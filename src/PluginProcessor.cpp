#include "PluginProcessor.h"

#include "PluginEditor.h"

#include <algorithm>

// M5: the sound is a PatchProgram built off the audio thread from a
// .pulsar patch (SPEC 3, 7, 9.3). Threads:
//   message thread  posts load jobs (mutex-protected deque; never touched
//                   by the audio thread), applies results (macro defaults,
//                   labels, status) through AsyncUpdater.
//   worker          parse -> offline matrix (Load only) -> compile ->
//                   push PatchProgram* on the SPSC swap queue; frees
//                   programs from the SPSC retire queue.
//   audio thread    pops the swap queue, fades out 5 ms, swaps, fades in
//                   5 ms, pushes the old program on the retire queue.
//
// Voice stealing (SPEC 9.1, amended): at most 8 voices SOUND at once
// (Active, or Fading with a queued note). A 9th note-on gives the steal
// victim (the oldest released voice, else the oldest held one) a 2 ms
// linear fade and starts the new note at once on a free spare. Only when no
// spare is free is a voice hard-stolen: it fades for 2 ms in place, then its
// rack is reset and the queued note starts. A voice is freed by
// VoiceLifetime (-80 dB for 50 ms after gate-off, 10 s cap) or when its
// steal fade ends.
//
// Real-time rules (SPEC 11): processBlock performs no allocation, locks,
// logging, I/O or exceptions, and never frees a program. MIDI is parsed
// from raw bytes (no MidiMessage copies).

namespace
{
// "-4.2 dB" with a real minus sign, for the status line.
juce::String formatDb (double gain)
{
    const double db = gain > 0.0 ? 20.0 * std::log10 (gain) : -100.0;
    auto s = juce::String (std::abs (db) < 0.05 ? 0.0 : db, 1);
    return s.replace ("-", juce::String::fromUTF8 ("\xe2\x88\x92")) + " dB";
}
} // namespace

//==============================================================================
VibeSynthAudioProcessor::VibeSynthAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VibeSynthParameters", createParameterLayout())
{
    for (int i = 0; i < numMacros; ++i)
        macroParams[(size_t) i] = apvts.getRawParameterValue ("macro" + juce::String (i + 1));
    for (int i = 0; i < numVoiceObjects; ++i)
        voices[(size_t) i].index = i;

    // The default patch (SPEC 9.4: reese.pulsar). Compiled in the first
    // prepareToPlay with its stored trim (no matrix; it is a built-in).
    const auto parsed = vibesynth::parsePatch (vibesynth::builtinPresetText (0));
    if (parsed.ok())
        defaultPatch = parsed.patch;
    else
        defaultPatch.voice = "out = 0;"; // unreachable unless the embed is broken
    activePatch = defaultPatch;
    for (int i = 0; i < numMacros; ++i)
        macroLabels[(size_t) i] = juce::String (i < (int) defaultPatch.macros.size()
                                                    ? defaultPatch.macros[(size_t) i].label
                                                    : std::string());
    statusText = "Loaded " + juce::String (defaultPatch.name);

    worker.startThread();
}

VibeSynthAudioProcessor::~VibeSynthAudioProcessor()
{
    // Stop the worker first (a validation in progress aborts at its next
    // 64-sample check), then free everything it or the audio thread owned.
    cancelValidation = true;
    worker.signalThreadShouldExit();
    worker.notify();
    worker.stopThread (10000);
    cancelPendingUpdate();

    std::lock_guard<std::mutex> lock (buildMutex);
    while (auto* p = swapQueue.pop())
        delete p;
    freeRetired();
    for (auto*& p : retireStash)
    {
        delete p;
        p = nullptr;
    }
    delete heldProgram;
    delete incoming;
    delete program;
    heldProgram = incoming = program = nullptr;
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout VibeSynthAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (int i = 1; i <= numMacros; ++i)
    {
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            "macro" + juce::String (i),
            "Macro " + juce::String (i),
            juce::NormalisableRange<float> (0.0f, 1.0f),
            0.5f));
    }

    return layout;
}

//==============================================================================
const juce::String VibeSynthAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool VibeSynthAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool VibeSynthAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool VibeSynthAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double VibeSynthAudioProcessor::getTailLengthSeconds() const
{
    // The built-in presets' releases reach -80 dB in about 0.45 s.
    return 0.5;
}

//==============================================================================
// Host programs = the built-in presets.

int VibeSynthAudioProcessor::getNumPrograms()
{
    return vibesynth::numBuiltinPresets();
}

int VibeSynthAudioProcessor::getCurrentProgram()
{
    return currentProgramIndex.load();
}

void VibeSynthAudioProcessor::setCurrentProgram (int index)
{
    // Re-selecting the current program is ignored: hosts re-send it after a
    // state restore, and reloading would reset the restored macro values.
    if (index < 0 || index >= getNumPrograms() || index == currentProgramIndex.load())
        return;
    loadBuiltin (index);
}

const juce::String VibeSynthAudioProcessor::getProgramName (int index)
{
    return juce::String (vibesynth::builtinPresetName (index));
}

void VibeSynthAudioProcessor::changeProgramName (int, const juce::String&) {}

//==============================================================================
// Loading (message thread, or any non-audio thread for the host API).

void VibeSynthAudioProcessor::loadBuiltin (int index)
{
    if (index < 0 || index >= vibesynth::numBuiltinPresets())
        return;
    currentProgramIndex = index;
    LoadJob job;
    job.kind = LoadJob::Kind::Load;
    job.text = vibesynth::builtinPresetText (index);
    job.source = juce::String (vibesynth::builtinPresetName (index));
    postJob (std::move (job));
}

void VibeSynthAudioProcessor::loadPatchFile (const juce::File& file)
{
    // Refuse oversize files before reading them; parsePatch re-checks.
    if (file.getSize() > (juce::int64) vibesynth::Patch::kMaxFileBytes)
    {
        setStatus ("Rejected " + file.getFileName() + ": patch file too large (max 256 KB)");
        return;
    }
    LoadJob job;
    job.kind = LoadJob::Kind::Load;
    job.text = file.loadFileAsString().toStdString();
    job.source = file.getFileName();
    postJob (std::move (job));
}

std::string VibeSynthAudioProcessor::getActivePatchText() const
{
    std::lock_guard<std::mutex> lock (patchMutex);
    return vibesynth::serializePatch (activePatch);
}

juce::String VibeSynthAudioProcessor::getActivePatchName() const
{
    std::lock_guard<std::mutex> lock (patchMutex);
    return juce::String (activePatch.name);
}

juce::String VibeSynthAudioProcessor::getMacroLabel (int index) const
{
    if (index < 0 || index >= numMacros)
        return {};
    const auto& label = macroLabels[(size_t) index];
    return label.isNotEmpty() ? label : "Macro " + juce::String (index + 1);
}

void VibeSynthAudioProcessor::setStatus (const juce::String& s)
{
    statusText = s;
    sendChangeMessage();
}

void VibeSynthAudioProcessor::postJob (LoadJob job)
{
    {
        // Under the lock, so takeJob() can't clear the flag for this job.
        std::lock_guard<std::mutex> lock (jobMutex);
        job.generation = ++generation;
        cancelValidation = true; // the job in progress (if any) is now stale
        jobs.push_back (std::move (job));
    }
    worker.notify();
}

bool VibeSynthAudioProcessor::takeJob (LoadJob& job)
{
    // Newest wins: older queued jobs are superseded and dropped.
    std::lock_guard<std::mutex> lock (jobMutex);
    if (jobs.empty())
        return false;
    job = std::move (jobs.back());
    jobs.clear();
    cancelValidation = false;
    return true;
}

//==============================================================================
// Worker thread.

void VibeSynthAudioProcessor::Worker::run()
{
    while (! threadShouldExit())
    {
        {
            std::lock_guard<std::mutex> lock (owner.buildMutex);
            owner.freeRetired();
            // A program the full swap queue refused earlier.
            if (owner.heldProgram != nullptr && owner.swapQueue.push (owner.heldProgram))
                owner.heldProgram = nullptr;
        }

        LoadJob job;
        if (owner.takeJob (job))
            owner.processJob (job);
        else
            wait (20);
    }
}

void VibeSynthAudioProcessor::freeRetired() noexcept
{
    while (auto* p = retireQueue.pop())
        delete p;
}

bool VibeSynthAudioProcessor::buildAndPushLocked (const vibesynth::Patch& patch, std::string& error)
{
    const double rate = preparedRate.load();
    if (rate <= 0.0)
    {
        // Not prepared yet: prepareToPlay compiles it at the real rate.
        pendingBuild = std::make_unique<vibesynth::Patch> (patch);
        return true;
    }
    auto built = PatchProgram::build (patch, rate, error);
    if (built == nullptr)
        return false;
    // Superseded by a newer program that never reached the audio thread.
    delete heldProgram;
    heldProgram = nullptr;
    auto* p = built.release();
    if (! swapQueue.push (p))
        heldProgram = p; // audio thread not running; retried by the worker loop
    return true;
}

void VibeSynthAudioProcessor::processJob (const LoadJob& job)
{
    const bool isLoad = job.kind == LoadJob::Kind::Load;
    auto stale = [this, &job] { return job.generation != generation.load(); };
    auto fail = [this, &job] (const juce::String& reason)
    {
        LoadResult r;
        r.message = "Rejected " + job.source + ": " + reason;
        r.generation = job.generation;
        postResult (std::move (r));
    };

    const auto parsed = vibesynth::parsePatch (job.text);
    vibesynth::Patch patch;
    juce::String restoreError;
    if (parsed.ok())
    {
        patch = parsed.patch;
    }
    else if (isLoad)
    {
        fail (parsed.error);
        return;
    }
    else
    {
        restoreError = parsed.error;
        patch = defaultPatch;
    }

    if (isLoad)
    {
        const double rate = preparedRate.load();
        const auto vr = vibesynth::validatePatch (patch, rate > 0.0 ? rate : 48000.0, &cancelValidation);
        if (vr.cancelled || stale())
            return;
        if (! vr.ok())
        {
            fail (vr.error);
            return;
        }
        patch.trim = vr.trim;
    }

    {
        std::lock_guard<std::mutex> lock (buildMutex);
        if (stale())
            return;
        std::string error;
        if (! buildAndPushLocked (patch, error))
        {
            if (isLoad)
            {
                fail (error);
                return;
            }
            // SPEC 9.3: a restored patch that doesn't compile -> default.
            restoreError = error;
            patch = defaultPatch;
            if (! buildAndPushLocked (patch, error))
                return;
        }
        std::lock_guard<std::mutex> plock (patchMutex);
        activePatch = patch;
    }

    LoadResult r;
    r.ok = true;
    r.patch = patch;
    r.setMacros = isLoad;
    r.generation = job.generation;
    if (restoreError.isNotEmpty())
        r.message = "Saved patch failed (" + restoreError + "); loaded " + juce::String (patch.name);
    else
        r.message = (isLoad ? "Loaded " : "Restored ") + juce::String (patch.name)
                  + ", trim " + formatDb (patch.trim);
    postResult (std::move (r));
}

void VibeSynthAudioProcessor::postResult (LoadResult result)
{
    {
        std::lock_guard<std::mutex> lock (resultMutex);
        results.push_back (std::move (result));
    }
    triggerAsyncUpdate();
}

// Message thread: macro defaults (SPEC 9.2), labels, status line.
void VibeSynthAudioProcessor::handleAsyncUpdate()
{
    std::vector<LoadResult> done;
    {
        std::lock_guard<std::mutex> lock (resultMutex);
        done.swap (results);
    }
    for (auto& r : done)
    {
        statusText = r.message;
        if (! r.ok)
            continue;
        for (int i = 0; i < numMacros; ++i)
            macroLabels[(size_t) i] = juce::String (i < (int) r.patch.macros.size()
                                                        ? r.patch.macros[(size_t) i].label
                                                        : std::string());
        // Only the newest load sets macros; a later restore or load wins.
        if (r.setMacros && r.generation == generation.load())
            for (int i = 0; i < numMacros; ++i)
                if (auto* param = apvts.getParameter ("macro" + juce::String (i + 1)))
                    param->setValueNotifyingHost ((float) r.patch.macroDefault (i));
    }
    if (! done.empty())
    {
        updateHostDisplay (ChangeDetails().withProgramChanged (true));
        sendChangeMessage();
    }
}

//==============================================================================
void VibeSynthAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Off the audio thread (and processBlock is not running): compile and
    // size everything here. Any sample rate or block size works (pluginval
    // exercises both); hosts that send larger blocks than announced are
    // handled in chunks.
    std::lock_guard<std::mutex> lock (buildMutex);

    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int block = samplesPerBlock > 0 ? samplesPerBlock : 512;
    currentSampleRate = rate;
    chunkCapacity = block;
    voiceSum.assign ((size_t) chunkCapacity, 0.0f);
    macroBlock.assign ((size_t) chunkCapacity * (size_t) numMacros, 0.0);

    // Take over everything in flight: the newest pushed program is the
    // patch to keep; the rest is freed (no audio thread is running).
    if (heldProgram != nullptr)
    {
        delete incoming;
        incoming = heldProgram;
        heldProgram = nullptr;
    }
    while (auto* p = swapQueue.pop())
    {
        delete incoming;
        incoming = p;
    }
    if (incoming != nullptr)
    {
        delete program;
        program = incoming;
        incoming = nullptr;
    }
    freeRetired();
    for (auto*& p : retireStash)
    {
        delete p;
        p = nullptr;
    }

    // Rebuild (compile only, as on restore) for a new rate or block size,
    // or compile a patch that arrived before the first prepare.
    const bool changed = ! juce::exactlyEqual (rate, preparedRate.load()) || block != preparedBlockSize;
    if (pendingBuild != nullptr || program == nullptr || changed)
    {
        const vibesynth::Patch patch = pendingBuild != nullptr ? *pendingBuild
                                     : program != nullptr      ? program->patch()
                                                               : defaultPatch;
        pendingBuild.reset();
        std::string error;
        auto built = PatchProgram::build (patch, rate, error);
        if (built == nullptr)
        {
            // SPEC 9.3: the saved patch doesn't compile -> default patch.
            const juce::String reason (error);
            built = PatchProgram::build (defaultPatch, rate, error);
            LoadResult r;
            r.ok = built != nullptr;
            r.patch = defaultPatch;
            r.message = "Saved patch failed (" + reason + "); loaded "
                      + juce::String (defaultPatch.name);
            r.generation = generation.load();
            postResult (std::move (r));
            std::lock_guard<std::mutex> plock (patchMutex);
            activePatch = defaultPatch;
        }
        delete program;
        program = built.release();
        if (program == nullptr)
            juce::Logger::writeToLog ("Pulsar: the default patch failed to compile: " + juce::String (error));
    }
    preparedRate = rate;
    preparedBlockSize = block;

    resetVoiceSlots();
    fade = Fade::None;
    fadeGain = 1.0f;
    fadeStep = 1.0f / (float) std::max (1, (int) std::lround (swapFadeSeconds * rate));
    guardMuteLength = std::max (1, (int) std::lround (guardMuteSeconds * rate));
    masterMuteSamples = 0;
    outputStage.init (rate);

    float targets[numMacros] = {};
    for (int m = 0; m < numMacros; ++m)
        targets[m] = macroParams[(size_t) m] != nullptr ? macroParams[(size_t) m]->load() : 0.5f;
    smoother.init (rate);
    smoother.snapAll (targets);
    noteCounter = 0;
}

void VibeSynthAudioProcessor::releaseResources()
{
    // Programs and buffers stay allocated until the next prepareToPlay
    // (or the destructor).
}

bool VibeSynthAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
}

void VibeSynthAudioProcessor::resetVoiceSlots() noexcept
{
    for (auto& v : voices)
    {
        const int index = v.index;
        v = VoiceSlot();
        v.index = index;
    }
}

//==============================================================================
// Swap protocol (audio thread; SPEC 7.3). No allocation, no frees.

void VibeSynthAudioProcessor::retire (PatchProgram* p) noexcept
{
    if (p == nullptr)
        return;
    if (retireQueue.push (p))
        return;
    // Queue full (the worker is stalled): park it and retry next block.
    for (auto*& slot : retireStash)
        if (slot == nullptr)
        {
            slot = p;
            return;
        }
    // Unreachable in practice (64 + 8 programs pending); leaking is the
    // only option that keeps the audio thread from freeing.
}

void VibeSynthAudioProcessor::flushRetireStash() noexcept
{
    for (auto*& slot : retireStash)
        if (slot != nullptr && retireQueue.push (slot))
            slot = nullptr;
}

void VibeSynthAudioProcessor::pollSwapQueue() noexcept
{
    // Several arrivals: keep the newest, retire the skipped ones.
    while (auto* p = swapQueue.pop())
    {
        retire (incoming);
        incoming = p;
    }
    if (incoming == nullptr)
        return;
    if (program == nullptr)
    {
        // Nothing playing: swap now and fade in.
        fadeGain = 0.0f;
        swapPrograms();
        return;
    }
    fade = Fade::Out; // also reverses a fade-in that was still running
}

int VibeSynthAudioProcessor::samplesUntilSilent() const noexcept
{
    return std::max (1, (int) std::ceil (fadeGain / fadeStep - 1.0e-4f));
}

void VibeSynthAudioProcessor::swapPrograms() noexcept
{
    retire (program);
    program = incoming;
    incoming = nullptr;
    masterMuteSamples = 0;

    // Held notes restart on their new voice (trig = 1, init runs);
    // released and fading voices are dropped, except a queued hard-steal
    // note, which starts now.
    for (auto& v : voices)
    {
        const bool held = v.state == VoiceSlot::State::Active
                          && (v.lifetime.gateHeld || v.releasePending);
        if (held)
        {
            v.lifetime.noteOn();
            v.muteSamples = 0;
            v.renderedSinceNoteOn = false;
            voiceOf (v).noteOn (v.freq, static_cast<double> (v.note), v.vel);
        }
        else if (v.state == VoiceSlot::State::Fading && v.hasPending)
        {
            const bool released = v.pendingReleased;
            beginNote (v, v.pendingNote, v.pendingVel);
            v.releasePending = released;
        }
        else
        {
            const int index = v.index;
            v = VoiceSlot();
            v.index = index;
        }
    }
    fade = Fade::In;
}

//==============================================================================
// Voice management (audio thread; no allocation).

VibeSynthAudioProcessor::VoiceSlot* VibeSynthAudioProcessor::findFree() noexcept
{
    for (auto& v : voices)
        if (v.state == VoiceSlot::State::Free)
            return &v;
    return nullptr;
}

VibeSynthAudioProcessor::VoiceSlot*
VibeSynthAudioProcessor::oldestWhere (bool (*pred) (const VoiceSlot&)) noexcept
{
    VoiceSlot* best = nullptr;
    for (auto& v : voices)
        if (pred (v) && (best == nullptr || v.startOrder < best->startOrder))
            best = &v;
    return best;
}

VibeSynthAudioProcessor::VoiceSlot* VibeSynthAudioProcessor::stealVictim() noexcept
{
    // A note-off still waiting for its note-on sample counts as released.
    if (auto* v = oldestWhere ([] (const VoiceSlot& s)
                               { return s.state == VoiceSlot::State::Active
                                        && (! s.lifetime.gateHeld || s.releasePending); }))
        return v;
    return oldestWhere ([] (const VoiceSlot& s) { return s.state == VoiceSlot::State::Active; });
}

void VibeSynthAudioProcessor::beginNote (VoiceSlot& v, int note, double velocity) noexcept
{
    v.state = VoiceSlot::State::Active;
    v.note = note;
    v.freq = juce::MidiMessage::getMidiNoteInHertz (note);
    v.vel = velocity;
    v.startOrder = ++noteCounter;
    v.renderedSinceNoteOn = false;
    v.releasePending = false;
    v.hasPending = false;
    v.pendingReleased = false;
    v.muteSamples = 0;
    v.lifetime.noteOn();
    voiceOf (v).noteOn (v.freq, static_cast<double> (note), velocity);
}

void VibeSynthAudioProcessor::beginFade (VoiceSlot& v) noexcept
{
    v.state = VoiceSlot::State::Fading;
    v.releasePending = false;
    v.lifetime.startStealFade (currentSampleRate);
}

void VibeSynthAudioProcessor::startNote (int note, double velocity) noexcept
{
    using State = VoiceSlot::State;

    if (program == nullptr)
        return;

    int sounding = 0;
    for (const auto& v : voices)
        if (v.state == State::Active || (v.state == State::Fading && v.hasPending))
            ++sounding;

    if (sounding >= maxSoundingVoices)
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

void VibeSynthAudioProcessor::stopNote (int note) noexcept
{
    for (auto& v : voices)
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

void VibeSynthAudioProcessor::allNotesOff() noexcept
{
    for (auto& v : voices)
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

void VibeSynthAudioProcessor::allSoundOff() noexcept
{
    for (auto& v : voices)
    {
        if (v.state == VoiceSlot::State::Active)
            beginFade (v);
        v.hasPending = false;
    }
}

void VibeSynthAudioProcessor::handleMidi (const juce::uint8* data, int numBytes) noexcept
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
void VibeSynthAudioProcessor::tripVoice (VoiceSlot& v) noexcept
{
    voiceOf (v).resetState();
    const bool held = v.state == VoiceSlot::State::Active
                      && (v.lifetime.gateHeld || v.releasePending);
    if (held)
    {
        voiceOf (v).noteOn (v.freq, static_cast<double> (v.note), v.vel);
        v.lifetime.noteOn();
        v.renderedSinceNoteOn = false;
        v.muteSamples = guardMuteLength;
    }
    else if (v.state == VoiceSlot::State::Fading && v.hasPending)
    {
        const bool released = v.pendingReleased;
        beginNote (v, v.pendingNote, v.pendingVel);
        v.releasePending = released;
        v.muteSamples = guardMuteLength;
    }
    else
    {
        const int index = v.index;
        v = VoiceSlot();
        v.index = index;
    }
}

void VibeSynthAudioProcessor::renderVoices (int start, int end) noexcept
{
    for (auto& v : voices)
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
                const int index = v.index;
                v = VoiceSlot();
                v.index = index;
                continue;
            }
        }

        auto& eel = voiceOf (v);
        auto& lt = v.lifetime;
        for (int i = from; i < end; ++i)
        {
            const double* macros = macroBlock.data() + (size_t) i * (size_t) numMacros;
            const float s = eel.renderSample (v.freq, lt.gateHeld ? 1.0 : 0.0,
                                              static_cast<double> (v.note), v.vel, macros);
            if (! vibesynth::isFiniteBits (s))
            {
                tripVoice (v);
                break;
            }
            voiceSum[(size_t) i] += s * lt.fadeGain();
            v.renderedSinceNoteOn = true;
            if (v.releasePending)
            {
                v.releasePending = false;
                lt.noteOff();
            }

            if (! lt.track (s, currentSampleRate))
                continue;

            if (v.state == VoiceSlot::State::Fading && v.hasPending)
            {
                // Hard steal: the fade has reached 0. Reset the rack so the
                // patch's envelopes restart from 0 (no jump), then start the
                // queued note in place on the next sample.
                const bool released = v.pendingReleased;
                eel.rack().reset();
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

void VibeSynthAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                            juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    buffer.clear();

    if (chunkCapacity <= 0 || numChannels <= 0)
        return;

    flushRetireStash();

    float targets[numMacros] = {};
    for (int m = 0; m < numMacros; ++m)
        targets[m] = macroParams[(size_t) m] != nullptr
                         ? macroParams[(size_t) m]->load (std::memory_order_relaxed)
                         : 0.5f;

    float* outL = buffer.getWritePointer (0);
    float* outR = numChannels > 1 ? buffer.getWritePointer (1) : nullptr;

    auto midiIt = midiMessages.cbegin();
    const auto midiEnd = midiMessages.cend();

    int chunkStart = 0;
    while (chunkStart < numSamples)
    {
        pollSwapQueue();

        int n = std::min (chunkCapacity, numSamples - chunkStart);
        // A fade-out ends exactly on a chunk boundary, where the swap runs.
        if (fade == Fade::Out)
            n = std::min (n, samplesUntilSilent());

        for (int i = 0; i < n; ++i)
        {
            smoother.step (targets);
            std::copy (smoother.current, smoother.current + numMacros,
                       macroBlock.data() + (size_t) i * (size_t) numMacros);
        }
        std::fill (voiceSum.begin(), voiceSum.begin() + n, 0.0f);

        // Sample-accurate MIDI: render up to each event, then apply it.
        int pos = 0;
        for (; midiIt != midiEnd; ++midiIt)
        {
            const auto event = *midiIt;
            const int at = std::max (event.samplePosition - chunkStart, pos);
            if (at >= n)
                break;
            if (at > pos && program != nullptr)
            {
                renderVoices (pos, at);
                pos = at;
            }
            handleMidi (event.data, event.numBytes);
        }
        if (pos < n && program != nullptr)
            renderVoices (pos, n);

        for (int i = 0; i < n; ++i)
        {
            float l = 0.0f, r = 0.0f;
            if (program != nullptr)
            {
                if (masterMuteSamples > 0)
                {
                    --masterMuteSamples;
                }
                else
                {
                    const float in = voiceSum[(size_t) i];
                    program->master().renderSample (in, in, l, r,
                                                    macroBlock.data() + (size_t) i * (size_t) numMacros);
                    if (! vibesynth::isFiniteBits (l) || ! vibesynth::isFiniteBits (r))
                    {
                        // Runtime guard for the master VM (same as a voice).
                        program->master().resetState();
                        masterMuteSamples = guardMuteLength;
                        l = r = 0.0f;
                    }
                }
                outputStage.process (l, r, program->trim(), l, r);
            }

            if (fade == Fade::Out)
            {
                fadeGain = std::max (0.0f, fadeGain - fadeStep);
            }
            else if (fade == Fade::In)
            {
                fadeGain = std::min (1.0f, fadeGain + fadeStep);
                if (fadeGain >= 1.0f)
                    fade = Fade::None;
            }
            l *= fadeGain;
            r *= fadeGain;

            if (outR != nullptr)
            {
                outL[chunkStart + i] = l;
                outR[chunkStart + i] = r;
            }
            else
            {
                outL[chunkStart + i] = 0.5f * (l + r);
            }
        }

        chunkStart += n;
        if (fade == Fade::Out && fadeGain <= 0.0f && incoming != nullptr)
            swapPrograms();
    }

    // Events stamped past the block end still apply (no stuck notes).
    for (; midiIt != midiEnd; ++midiIt)
    {
        const auto event = *midiIt;
        handleMidi (event.data, event.numBytes);
    }
}

//==============================================================================
bool VibeSynthAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* VibeSynthAudioProcessor::createEditor()
{
    return new VibeSynthAudioProcessorEditor (*this);
}

//==============================================================================
// State (SPEC 9.3): the APVTS tree with the active patch's JSON and the
// host program index as properties. Older or newer states fail soft.

void VibeSynthAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    {
        std::lock_guard<std::mutex> lock (patchMutex);
        state.setProperty ("patchJson", juce::String (vibesynth::serializePatch (activePatch)), nullptr);
    }
    state.setProperty ("program", currentProgramIndex.load(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void VibeSynthAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);
    const auto json = tree.getProperty ("patchJson").toString();
    const int programIndex = tree.getProperty ("program", -1);
    tree.removeProperty ("patchJson", nullptr);
    tree.removeProperty ("program", nullptr);
    apvts.replaceState (tree);

    if (programIndex >= 0 && programIndex < getNumPrograms())
        currentProgramIndex = programIndex;
    if (json.isEmpty())
        return; // an older state without a patch: keep the current one

    // Compile on the worker (or in prepareToPlay if not prepared yet),
    // never here: this may run on any thread pluginval picks.
    const auto parsed = vibesynth::parsePatch (json.toStdString());
    if (parsed.ok())
    {
        std::lock_guard<std::mutex> lock (patchMutex);
        activePatch = parsed.patch;
    }

    {
        std::lock_guard<std::mutex> lock (buildMutex);
        if (preparedRate.load() <= 0.0 && parsed.ok())
        {
            generation++; // supersede any load still in flight
            pendingBuild = std::make_unique<vibesynth::Patch> (parsed.patch);
            LoadResult r;
            r.ok = true;
            r.patch = parsed.patch;
            r.message = "Restored " + juce::String (parsed.patch.name)
                      + ", trim " + formatDb (parsed.patch.trim);
            r.generation = generation.load();
            postResult (std::move (r));
            return;
        }
    }

    LoadJob job;
    job.kind = LoadJob::Kind::Restore;
    job.text = json.toStdString();
    job.source = "saved patch";
    postJob (std::move (job));
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VibeSynthAudioProcessor();
}
