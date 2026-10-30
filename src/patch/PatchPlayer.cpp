// PatchPlayer: the plugin's voice + master chain, shared with
// vibesynth_cli --render. See PatchPlayer.h. Moved from PluginProcessor.cpp
// in M6 without behaviour changes.

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
    voiceSum_.assign ((size_t) chunkCapacity_, 0.0f);
    macroBlock_.assign ((size_t) chunkCapacity_ * (size_t) kNumMacros, 0.0);
    outL_.assign ((size_t) chunkCapacity_, 0.0f);
    outR_.assign ((size_t) chunkCapacity_, 0.0f);

    for (auto& v : voices_)
        clearSlot (v);
    guardMuteLength_ = std::max (1, (int) std::lround (kGuardMuteSeconds * sampleRate_));
    masterMuteSamples_ = 0;
    outputStage_.init (sampleRate_);
    smoother_.init (sampleRate_);
    smoother_.snapAll (macroTargets);
    noteCounter_ = 0;
    transport_ = Transport();
    eventTransport_ = transport_;
    chunkBeat_ = 0.0;
    beatStep_ = transport_.beatStep (sampleRate_);
}

void PatchPlayer::setHostTransport (double bpm, bool playing, bool hasPpq, double ppq) noexcept
{
    // Clamped so patches can divide by it and LFOs stay sane.
    transport_.bpm = (isFiniteBits (bpm) && bpm > 0.0) ? std::clamp (bpm, 20.0, 999.0)
                                                         : Transport::kDefaultBpm;
    if (playing && hasPpq && isFiniteBits (ppq))
        transport_.beat = ppq;
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
    if (program_ == nullptr)
    {
        for (auto& v : voices_)
            clearSlot (v);
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
            voiceOf (v).noteOn (v.freq, static_cast<double> (v.note), v.vel, transport_);
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
    v.vel = velocity;
    v.startOrder = ++noteCounter_;
    v.renderedSinceNoteOn = false;
    v.releasePending = false;
    v.hasPending = false;
    v.pendingReleased = false;
    v.muteSamples = 0;
    v.lifetime.noteOn();
    voiceOf (v).noteOn (v.freq, static_cast<double> (note), velocity, eventTransport_);
}

void PatchPlayer::beginFade (VoiceSlot& v) noexcept
{
    v.state = VoiceSlot::State::Fading;
    v.releasePending = false;
    v.lifetime.startStealFade (sampleRate_);
}

void PatchPlayer::startNote (int note, double velocity) noexcept
{
    using State = VoiceSlot::State;

    if (program_ == nullptr)
        return;

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
        return;
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
        return;

    target->hasPending = true;
    target->pendingNote = note;
    target->pendingVel = velocity;
    target->pendingReleased = false;
}

void PatchPlayer::stopNote (int note) noexcept
{
    for (auto& v : voices_)
    {
        if (v.state == VoiceSlot::State::Active && v.note == note && v.lifetime.gateHeld)
        {
            // The patch must see gate = 1 for at least the note-on sample,
            // or its envelope never gets a rising edge.
            if (v.renderedSinceNoteOn)
                v.lifetime.noteOff();
            else
                v.releasePending = true;
        }
        else if (v.state == VoiceSlot::State::Fading && v.hasPending && v.pendingNote == note)
        {
            v.pendingReleased = true;
        }
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
}

void PatchPlayer::allSoundOff() noexcept
{
    for (auto& v : voices_)
    {
        if (v.state == VoiceSlot::State::Active)
            beginFade (v);
        v.hasPending = false;
    }
}

void PatchPlayer::handleMidi (const uint8_t* data, int numBytes) noexcept
{
    if (data == nullptr || numBytes < 3)
        return;
    const int type = data[0] & 0xF0;
    const int d1 = data[1] & 0x7F;
    const int d2 = data[2] & 0x7F;

    if (type == 0x90 && d2 > 0)
        startNote (d1, static_cast<double> (d2) / 127.0);
    else if (type == 0x80 || type == 0x90)
        stopNote (d1);
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
    if (held)
    {
        voiceOf (v).noteOn (v.freq, static_cast<double> (v.note), v.vel, eventTransport_);
        v.lifetime.noteOn();
        v.renderedSinceNoteOn = false;
        v.muteSamples = guardMuteLength_;
    }
    else if (v.state == VoiceSlot::State::Fading && v.hasPending)
    {
        const bool released = v.pendingReleased;
        beginNote (v, v.pendingNote, v.pendingVel);
        v.releasePending = released;
        v.muteSamples = guardMuteLength_;
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
    std::fill (voiceSum_.begin(), voiceSum_.begin() + n, 0.0f);
}

void PatchPlayer::renderVoices (int start, int end) noexcept
{
    for (auto& v : voices_)
    {
        if (v.state == VoiceSlot::State::Free)
            continue;

        int from = start;
        if (v.muteSamples > 0)
        {
            const int skip = std::min (v.muteSamples, end - start);
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
        for (int i = from; i < end; ++i)
        {
            const double* macros = macroBlock_.data() + (size_t) i * (size_t) kNumMacros;
            t.beat = chunkBeat_ + i * beatStep_;
            const float s = eel.renderSample (v.freq, lt.gateHeld ? 1.0 : 0.0,
                                              static_cast<double> (v.note), v.vel, macros, t);
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

            if (! lt.track (s, sampleRate_))
                continue;

            if (v.state == VoiceSlot::State::Fading && v.hasPending)
            {
                // Hard steal: the fade has reached 0. Reset the rack so the
                // patch's envelopes restart from 0 (no jump), then start the
                // queued note in place on the next sample.
                const bool released = v.pendingReleased;
                eel.rack().reset();
                eventTransport_.beat = chunkBeat_ + (i + 1) * beatStep_;
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
                                                 macroBlock_.data() + (size_t) i * (size_t) kNumMacros, t);
                if (! isFiniteBits (l) || ! isFiniteBits (r))
                {
                    // Runtime guard for the master VM (same as a voice).
                    program_->master().resetState();
                    masterMuteSamples_ = guardMuteLength_;
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
}

} // namespace vibesynth
