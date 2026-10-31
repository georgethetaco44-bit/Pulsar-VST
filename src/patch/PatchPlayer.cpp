// PatchPlayer: the plugin's voice + master chain, shared with
// vibesynth_cli --render. See PatchPlayer.h. Moved from PluginProcessor.cpp
// in M6 without behaviour changes; M7 added the 2x voice path, voice
// modes, glide, pitch bend and the mod wheel.

#include "patch/PatchPlayer.h"

#include <cmath>

namespace vibesynth {

PatchPlayer::PatchPlayer()
{
    for (int i = 0; i < kNumVoiceObjects; ++i)
        voices_[(size_t) i].index = i;
}

double PatchPlayer::noteHz (int note) noexcept
{
    // Same formula as juce::MidiMessage::getMidiNoteInHertz (A4 = 440 Hz).
    return 440.0 * std::pow (2.0, (note - 69) / 12.0);
}

void PatchPlayer::prepare (double sampleRate, int maxChunk, const float macroTargets[kNumMacros])
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    chunkCapacity_ = maxChunk > 0 ? maxChunk : 512;
    voiceSum_.assign ((size_t) chunkCapacity_ * (size_t) PatchProgram::kMaxOversampling, 0.0f);
    macroBlock_.assign ((size_t) chunkCapacity_ * (size_t) kNumMacros, 0.0);
    mwBlock_.assign ((size_t) chunkCapacity_, 0.0);
    bendBlock_.assign ((size_t) chunkCapacity_, 1.0);
    outL_.assign ((size_t) chunkCapacity_, 0.0f);
    outR_.assign ((size_t) chunkCapacity_, 0.0f);

    for (auto& v : voices_)
        clearSlot (v);
    masterMuteLength_ = std::max (1, (int) std::lround (kGuardMuteSeconds * sampleRate_));
    masterMuteSamples_ = 0;
    outputStage_.init (sampleRate_);
    decimator_.init();
    smoother_.init (sampleRate_);
    smoother_.snapAll (macroTargets);
    noteCounter_ = 0;
    transport_ = Transport();
    eventTransport_ = transport_;
    chunkBeat_ = 0.0;
    beatStep_ = transport_.beatStep (sampleRate_);

    monoActive_ = false;
    monoIndex_ = -1;
    clearHeld();
    mwCoeff_ = smoother_.coeff; // "smoothed like the macros" (SPEC §4.3)
    mwTarget_ = mwCurrent_ = eventMw_ = 0.0;
    bendCoeff_ = 1.0 - std::exp (-1.0 / (kPitchBendSeconds * sampleRate_));
    bendTarget_ = bendCurrent_ = 0.0;
    bendRatio_ = eventBend_ = 1.0;
}

void PatchPlayer::setHostTransport (double bpm, bool playing, bool hasPpq, double ppq) noexcept
{
    // Clamped so patches can divide by it and LFOs stay sane.
    transport_.bpm = (isFiniteBits (bpm) && bpm > 0.0) ? std::clamp (bpm, 20.0, 999.0)
                                                         : Transport::kDefaultBpm;
    if (playing && hasPpq && isFiniteBits (ppq))
        transport_.beat = ppq;
}

void PatchPlayer::setVoiceControls (VoiceMode mode, double glide) noexcept
{
    modeSetting_ = mode;
    glide_ = isFiniteBits (glide) ? std::clamp (glide, 0.0, 1.0) : 0.0;
}

void PatchPlayer::clearSlot (VoiceSlot& v) noexcept
{
    const int index = v.index;
    v = VoiceSlot();
    v.index = index;
}

PatchProgram* PatchPlayer::swapProgram (PatchProgram* next) noexcept
{
    PatchProgram* old = program_;
    program_ = next;
    masterMuteSamples_ = 0;
    decimator_.reset();
    if (program_ == nullptr)
    {
        for (auto& v : voices_)
            clearSlot (v);
        monoIndex_ = -1;
        clearHeld();
        return old;
    }
    program_->master().resetRacks();

    for (auto& v : voices_)
    {
        const bool held = v.state == VoiceSlot::State::Active
                          && (v.lifetime.gateHeld || v.releasePending);
        if (held)
        {
            v.lifetime.noteOn();
            v.muteSamples = 0;
            v.renderedSinceNoteOn = false;
            voiceOf (v).noteOn (v.freq * bendRatio_, static_cast<double> (v.note), v.vel,
                                transport_, mwCurrent_);
        }
        else if (v.state == VoiceSlot::State::Fading && v.hasPending)
        {
            const bool released = v.pendingReleased;
            beginNote (v, v.pendingNote, v.pendingVel);
            v.releasePending = released;
        }
        else
        {
            clearSlot (v);
        }
    }
    return old;
}

//==============================================================================
// Voice management (audio thread; no allocation).

PatchPlayer::VoiceSlot* PatchPlayer::findFree() noexcept
{
    for (auto& v : voices_)
        if (v.state == VoiceSlot::State::Free)
            return &v;
    return nullptr;
}

PatchPlayer::VoiceSlot* PatchPlayer::oldestWhere (bool (*pred) (const VoiceSlot&)) noexcept
{
    VoiceSlot* best = nullptr;
    for (auto& v : voices_)
        if (pred (v) && (best == nullptr || v.startOrder < best->startOrder))
            best = &v;
    return best;
}

PatchPlayer::VoiceSlot* PatchPlayer::stealVictim() noexcept
{
    // A note-off still waiting for its note-on sample counts as released.
    if (auto* v = oldestWhere ([] (const VoiceSlot& s)
                               { return s.state == VoiceSlot::State::Active
                                        && (! s.lifetime.gateHeld || s.releasePending); }))
        return v;
    return oldestWhere ([] (const VoiceSlot& s) { return s.state == VoiceSlot::State::Active; });
}

void PatchPlayer::beginNote (VoiceSlot& v, int note, double velocity) noexcept
{
    v.state = VoiceSlot::State::Active;
    v.note = note;
    v.freq = noteHz (note);
    v.glideLeft = 0;
    v.vel = velocity;
    v.startOrder = ++noteCounter_;
    v.renderedSinceNoteOn = false;
    v.releasePending = false;
    v.hasPending = false;
    v.pendingReleased = false;
    v.muteSamples = 0;
    v.lifetime.noteOn();
    voiceOf (v).noteOn (v.freq * eventBend_, static_cast<double> (note), velocity,
                        eventTransport_, eventMw_);
}

void PatchPlayer::beginFade (VoiceSlot& v) noexcept
{
    v.state = VoiceSlot::State::Fading;
    v.releasePending = false;
    v.lifetime.startStealFade (voiceRate());
}

// Poly allocation (SPEC 9.1). Returns the slot that plays the note: a
// spare that started it now, or a fading slot it is queued on.
PatchPlayer::VoiceSlot* PatchPlayer::startNote (int note, double velocity) noexcept
{
    using State = VoiceSlot::State;

    if (program_ == nullptr)
        return nullptr;

    int sounding = 0;
    for (const auto& v : voices_)
        if (v.state == State::Active || (v.state == State::Fading && v.hasPending))
            ++sounding;

    if (sounding >= kMaxSoundingVoices)
        if (auto* victim = stealVictim())
            beginFade (*victim);

    if (auto* spare = findFree())
    {
        beginNote (*spare, note, velocity);
        return spare;
    }

    // No spare: hard steal. Prefer a voice that is already fading with
    // nothing queued (its fade is shorter), else fade the steal victim in
    // place. With every voice fading and queued (10 note-ons within 2 ms)
    // the oldest queued note is replaced.
    VoiceSlot* target = oldestWhere ([] (const VoiceSlot& s)
                                     { return s.state == State::Fading && ! s.hasPending; });
    if (target == nullptr)
    {
        target = stealVictim();
        if (target != nullptr)
            beginFade (*target);
    }
    if (target == nullptr)
        target = oldestWhere ([] (const VoiceSlot& s) { return s.state == State::Fading; });
    if (target == nullptr)
        return nullptr;

    target->hasPending = true;
    target->pendingNote = note;
    target->pendingVel = velocity;
    target->pendingReleased = false;
    return target;
}

// Gate off for one slot. The patch must see gate = 1 for at least the
// note-on sample, or its envelope never gets a rising edge.
void PatchPlayer::releaseSlot (VoiceSlot& v) noexcept
{
    if (v.state == VoiceSlot::State::Active && v.lifetime.gateHeld)
    {
        if (v.renderedSinceNoteOn)
            v.lifetime.noteOff();
        else
            v.releasePending = true;
    }
    else if (v.state == VoiceSlot::State::Fading && v.hasPending)
    {
        v.pendingReleased = true;
    }
}

void PatchPlayer::stopNote (int note) noexcept
{
    for (auto& v : voices_)
    {
        if (v.state == VoiceSlot::State::Active && v.note == note && v.lifetime.gateHeld)
            releaseSlot (v);
        else if (v.state == VoiceSlot::State::Fading && v.hasPending && v.pendingNote == note)
            v.pendingReleased = true;
    }
}

void PatchPlayer::allNotesOff() noexcept
{
    for (auto& v : voices_)
    {
        if (v.state == VoiceSlot::State::Active && v.lifetime.gateHeld)
        {
            if (v.renderedSinceNoteOn)
                v.lifetime.noteOff();
            else
                v.releasePending = true;
        }
        v.pendingReleased = true;
    }
    clearHeld();
}

void PatchPlayer::allSoundOff() noexcept
{
    for (auto& v : voices_)
    {
        if (v.state == VoiceSlot::State::Active)
            beginFade (v);
        v.hasPending = false;
    }
    clearHeld();
    monoIndex_ = -1;
}

//==============================================================================
// Mono (M7, SPEC 9.1).

void PatchPlayer::pushHeld (int note) noexcept
{
    removeHeld (note);
    if (heldCount_ < (int) held_.size())
        held_[(size_t) heldCount_++] = static_cast<uint8_t> (note);
}

bool PatchPlayer::removeHeld (int note) noexcept
{
    for (int i = 0; i < heldCount_; ++i)
    {
        if (held_[(size_t) i] != note)
            continue;
        for (int j = i + 1; j < heldCount_; ++j)
            held_[(size_t) j - 1] = held_[(size_t) j];
        --heldCount_;
        return true;
    }
    return false;
}

// The mono voice, if it is still playing (Active) or about to (a queued
// hard-steal note).
PatchPlayer::VoiceSlot* PatchPlayer::monoSlot() noexcept
{
    if (monoIndex_ < 0)
        return nullptr;
    auto& v = voices_[(size_t) monoIndex_];
    if (v.state == VoiceSlot::State::Active || (v.state == VoiceSlot::State::Fading && v.hasPending))
        return &v;
    monoIndex_ = -1;
    return nullptr;
}

// Exponential in Hz (linear in pitch), constant time: glide^2 seconds.
void PatchPlayer::glideTo (VoiceSlot& v, int note) noexcept
{
    v.note = note;
    const double target = noteHz (note);
    const double seconds = glide_ * glide_ * kMaxGlideSeconds;
    const int n = (int) std::lround (seconds * voiceRate());
    if (n <= 0 || ! (v.freq > 0.0))
    {
        v.freq = target;
        v.glideLeft = 0;
        return;
    }
    v.glideTarget = target;
    v.glideRatio = std::exp2 (std::log2 (target / v.freq) / n);
    v.glideLeft = n;
}

void PatchPlayer::monoNoteOn (int note, double velocity) noexcept
{
    const bool legato = heldCount_ > 0;
    pushHeld (note);
    VoiceSlot* v = monoSlot();
    if (v == nullptr)
    {
        if (VoiceSlot* s = startNote (note, velocity))
            monoIndex_ = s->index;
        return;
    }
    if (v->state == VoiceSlot::State::Fading)
    {
        // Its note is still queued behind a hard-steal fade.
        v->pendingNote = note;
        if (! legato)
        {
            v->pendingVel = velocity;
            v->pendingReleased = false;
        }
        return;
    }
    if (legato)
    {
        // Glide, keep the gate, no trig, no init.
        glideTo (*v, note);
        return;
    }
    // All keys were up: retrigger in place (trig, init; no steal fade).
    v->note = note;
    v->freq = noteHz (note);
    v->glideLeft = 0;
    v->vel = velocity;
    v->startOrder = ++noteCounter_;
    v->renderedSinceNoteOn = false;
    v->releasePending = false;
    v->lifetime.noteOn();
    voiceOf (*v).noteOn (v->freq * eventBend_, static_cast<double> (note), velocity,
                         eventTransport_, eventMw_);
}

void PatchPlayer::monoNoteOff (int note) noexcept
{
    const bool wasTop = heldCount_ > 0 && held_[(size_t) heldCount_ - 1] == note;
    if (! removeHeld (note))
    {
        stopNote (note); // not ours (e.g. from before a mode change)
        return;
    }
    if (! wasTop)
        return;
    VoiceSlot* v = monoSlot();
    if (v == nullptr)
        return;
    if (heldCount_ > 0)
    {
        // Back to the newest key still held, legato.
        const int prev = held_[(size_t) heldCount_ - 1];
        if (v->state == VoiceSlot::State::Fading)
            v->pendingNote = prev;
        else
            glideTo (*v, prev);
        return;
    }
    releaseSlot (*v);
}

void PatchPlayer::handleMidi (const uint8_t* data, int numBytes) noexcept
{
    if (data == nullptr || numBytes < 3)
        return;
    const int type = data[0] & 0xF0;
    const int d1 = data[1] & 0x7F;
    const int d2 = data[2] & 0x7F;

    if (type == 0x90 && d2 > 0)
    {
        if (monoActive_)
            monoNoteOn (d1, static_cast<double> (d2) / 127.0);
        else
            startNote (d1, static_cast<double> (d2) / 127.0);
    }
    else if (type == 0x80 || type == 0x90)
    {
        if (monoActive_)
            monoNoteOff (d1);
        else
            stopNote (d1);
    }
    else if (type == 0xE0)
    {
        // 14-bit, centre 8192; ±kPitchBendRange semitones.
        const int value = (d2 << 7) | d1;
        bendTarget_ = kPitchBendRange * (value - 8192) / 8192.0;
    }
    else if (type == 0xB0 && d1 == 1)
        mwTarget_ = static_cast<double> (d2) / 127.0;
    else if (type == 0xB0 && d1 == 120)
        allSoundOff();
    else if (type == 0xB0 && d1 == 123)
        allNotesOff();
}

// Runtime guard (SPEC 7.2): the voice produced a non-finite sample. Reset
// its rack, VM variables and mem[]; a held note restarts (note-on now,
// first rendered after a 20 ms mute), anything else is dropped.
void PatchPlayer::tripVoice (VoiceSlot& v) noexcept
{
    voiceOf (v).resetState();
    const bool held = v.state == VoiceSlot::State::Active
                      && (v.lifetime.gateHeld || v.releasePending);
    const int mute = std::max (1, (int) std::lround (kGuardMuteSeconds * voiceRate()));
    if (held)
    {
        voiceOf (v).noteOn (v.freq * bendRatio_, static_cast<double> (v.note), v.vel,
                            eventTransport_, mwCurrent_);
        v.lifetime.noteOn();
        v.renderedSinceNoteOn = false;
        v.muteSamples = mute;
    }
    else if (v.state == VoiceSlot::State::Fading && v.hasPending)
    {
        const bool released = v.pendingReleased;
        beginNote (v, v.pendingNote, v.pendingVel);
        v.releasePending = released;
        v.muteSamples = mute;
    }
    else
    {
        clearSlot (v);
    }
}

//==============================================================================
// Rendering (audio thread).

void PatchPlayer::beginChunk (int n, const float targets[kNumMacros]) noexcept
{
    // The effective voice mode; a change releases every held note.
    const bool mono = modeSetting_ == VoiceMode::Mono
                      || (modeSetting_ == VoiceMode::Patch && program_ != nullptr
                          && program_->patch().voiceMode == "mono");
    if (mono != monoActive_)
    {
        allNotesOff();
        monoIndex_ = -1;
        monoActive_ = mono;
    }

    chunkBeat_ = transport_.beat;
    beatStep_ = transport_.beatStep (sampleRate_);
    eventTransport_.bpm = transport_.bpm;
    eventTransport_.beat = chunkBeat_;
    for (int i = 0; i < n; ++i)
    {
        smoother_.step (targets);
        std::copy (smoother_.current, smoother_.current + kNumMacros,
                   macroBlock_.data() + (size_t) i * (size_t) kNumMacros);
    }
    eventMw_ = mwCurrent_;
    eventBend_ = bendRatio_;
    std::fill (voiceSum_.begin(), voiceSum_.begin() + (size_t) n * (size_t) oversampling(), 0.0f);
}

// Per host sample, between MIDI events, so a bend or CC1 takes effect at
// its own sample (under its 5 ms / 10 ms smoothing).
void PatchPlayer::fillControls (int from, int to) noexcept
{
    for (int i = from; i < to; ++i)
    {
        mwCurrent_ += mwCoeff_ * (mwTarget_ - mwCurrent_);
        mwBlock_[(size_t) i] = mwCurrent_;
        const double d = bendTarget_ - bendCurrent_;
        if (d != 0.0)
        {
            bendCurrent_ = (d < 1.0e-7 && d > -1.0e-7) ? bendTarget_ : bendCurrent_ + bendCoeff_ * d;
            bendRatio_ = std::exp2 (bendCurrent_ / 12.0);
        }
        bendBlock_[(size_t) i] = bendRatio_;
    }
}

// [start, end) are host samples; each runs `os` voice samples.
void PatchPlayer::renderVoices (int start, int end) noexcept
{
    const int os = oversampling();
    const double vrate = voiceRate();
    const double voiceBeatStep = beatStep_ / os;
    const int vstart = start * os;
    const int vend = end * os;

    for (auto& v : voices_)
    {
        if (v.state == VoiceSlot::State::Free)
            continue;

        int from = vstart;
        if (v.muteSamples > 0)
        {
            const int skip = std::min (v.muteSamples, vend - vstart);
            v.muteSamples -= skip;
            from += skip;
            if (v.muteSamples > 0)
                continue;
            // Mute over: a note released meanwhile is dropped unheard.
            if (v.releasePending || ! v.lifetime.gateHeld)
            {
                clearSlot (v);
                continue;
            }
        }

        auto& eel = voiceOf (v);
        auto& lt = v.lifetime;
        Transport t = transport_;
        for (int i = from; i < vend; ++i)
        {
            const int h = i / os;
            const double* macros = macroBlock_.data() + (size_t) h * (size_t) kNumMacros;
            if (v.glideLeft > 0)
            {
                v.freq = --v.glideLeft > 0 ? v.freq * v.glideRatio : v.glideTarget;
            }
            t.beat = chunkBeat_ + i * voiceBeatStep;
            const float s = eel.renderSample (v.freq * bendBlock_[(size_t) h], lt.gateHeld ? 1.0 : 0.0,
                                              static_cast<double> (v.note), v.vel, macros, t,
                                              mwBlock_[(size_t) h]);
            if (! isFiniteBits (s))
            {
                tripVoice (v);
                break;
            }
            voiceSum_[(size_t) i] += s * lt.fadeGain();
            v.renderedSinceNoteOn = true;
            if (v.releasePending)
            {
                v.releasePending = false;
                lt.noteOff();
            }

            if (! lt.track (s, vrate))
                continue;

            if (v.state == VoiceSlot::State::Fading && v.hasPending)
            {
                // Hard steal: the fade has reached 0. Reset the rack so the
                // patch's envelopes restart from 0 (no jump), then start the
                // queued note in place on the next sample.
                const bool released = v.pendingReleased;
                eel.rack().reset();
                eventTransport_.beat = chunkBeat_ + (i + 1) * voiceBeatStep;
                beginNote (v, v.pendingNote, v.pendingVel);
                v.releasePending = released;
                continue;
            }

            v.state = VoiceSlot::State::Free;
            v.hasPending = false;
            break;
        }
    }
}

void PatchPlayer::finishChunk (int n) noexcept
{
    // Back to the host rate: one half-band decimator on the mono sum
    // (in place; output sample i only reads voice samples >= 2i).
    if (oversampling() == 2)
        for (int i = 0; i < n; ++i)
            voiceSum_[(size_t) i] = decimator_.process (voiceSum_[(size_t) (2 * i)],
                                                        voiceSum_[(size_t) (2 * i + 1)]);

    Transport t = transport_;
    for (int i = 0; i < n; ++i)
    {
        t.beat = chunkBeat_ + i * beatStep_;
        float l = 0.0f, r = 0.0f;
        if (program_ != nullptr)
        {
            if (masterMuteSamples_ > 0)
            {
                --masterMuteSamples_;
            }
            else
            {
                const float in = voiceSum_[(size_t) i];
                program_->master().renderSample (in, in, l, r,
                                                 macroBlock_.data() + (size_t) i * (size_t) kNumMacros, t,
                                                 mwBlock_[(size_t) i]);
                if (! isFiniteBits (l) || ! isFiniteBits (r))
                {
                    // Runtime guard for the master VM (same as a voice).
                    program_->master().resetState();
                    masterMuteSamples_ = masterMuteLength_;
                    l = r = 0.0f;
                }
            }
            outputStage_.process (l, r, program_->trim(), l, r);
        }
        outL_[(size_t) i] = l;
        outR_[(size_t) i] = r;
    }
    // Free-running between host updates (SPEC §4.3).
    transport_.beat = chunkBeat_ + n * beatStep_;
    eventTransport_.beat = transport_.beat; // late events (past the chunk)
    eventMw_ = mwCurrent_;
    eventBend_ = bendRatio_;
}

//==============================================================================
// Tests.

int PatchPlayer::slotVoiceIndex (int slot) const noexcept
{
    if (slot < 0 || slot >= kNumVoiceObjects)
        return -1;
    const auto& v = voices_[(size_t) slot];
    return v.state == VoiceSlot::State::Free ? -1 : v.index;
}

int PatchPlayer::slotNote (int slot) const noexcept
{
    return slotVoiceIndex (slot) < 0 ? -1 : voices_[(size_t) slot].note;
}

double PatchPlayer::slotFreq (int slot) const noexcept
{
    return slotVoiceIndex (slot) < 0 ? 0.0 : voices_[(size_t) slot].freq;
}

bool PatchPlayer::slotGate (int slot) const noexcept
{
    return slotVoiceIndex (slot) >= 0 && voices_[(size_t) slot].lifetime.gateHeld;
}

void PatchPlayer::harvestMeters (NodeMeterBank& bank) noexcept
{
    if (program_ == nullptr)
        return;
    bool sounding[kNumVoiceObjects] = {};
    for (const auto& v : voices_)
        if (v.state != VoiceSlot::State::Free && v.index >= 0 && v.index < kNumVoiceObjects)
            sounding[v.index] = true;
    for (int i = 0; i < kNumVoiceObjects; ++i)
    {
        auto& rack = program_->voice (i).rack();
        harvestRackMeters (rack.meters, rack.touched, sounding[i] ? bank.voice : nullptr);
    }
    auto& master = program_->master();
    harvestRackMeters (master.rack().meters, master.rack().touched, bank.master);
    harvestRackMeters (master.voiceRack().meters, master.voiceRack().touched, bank.master);
}

int PatchPlayer::soundingVoices() const noexcept
{
    int n = 0;
    for (const auto& v : voices_)
        if (v.state == VoiceSlot::State::Active || (v.state == VoiceSlot::State::Fading && v.hasPending))
            ++n;
    return n;
}

} // namespace vibesynth
