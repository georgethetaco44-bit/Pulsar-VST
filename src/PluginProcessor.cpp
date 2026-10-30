#include "PluginProcessor.h"

#include "PluginEditor.h"

#include <algorithm>

// M5: the sound is a PatchProgram built off the audio thread from a
// .pulsar patch (SPEC 3, 7, 9.3). Threads:
//   message thread  posts load jobs (mutex-protected deque; never touched
//                   by the audio thread), applies results (macro defaults,
//                   labels, status) through AsyncUpdater.
//   worker          parse -> offline matrix (file loads only) -> compile ->
//                   push PatchProgram* on the SPSC swap queue; frees
//                   programs from the SPSC retire queue.
//   audio thread    pops the swap queue, fades out 5 ms, swaps, fades in
//                   5 ms, pushes the old program on the retire queue.
//
// M6: voices, stealing, MIDI, the master VM, the runtime guard and the
// output stage are vibesynth::PatchPlayer (see PatchPlayer.h), shared with
// vibesynth_cli --render; this file keeps the threads and the swap fade.
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
    voiceModeParam = apvts.getRawParameterValue (voiceModeParamId);
    glideParam = apvts.getRawParameterValue (glideParamId);

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
    // Stop the worker first (a validation in progress aborts after its
    // current sample), then free everything it or the audio thread owned.
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
    delete player.program();
    player.setProgram (nullptr);
    heldProgram = incoming = nullptr;
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

    // M7 (SPEC 9.1): added after the macros, never removed. Choice order
    // matches vibesynth::PatchPlayer::VoiceMode.
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        voiceModeParamId, "Voice Mode", juce::StringArray { "Patch", "Poly", "Mono" }, 0));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        glideParamId, "Glide", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f));

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
    // The longest built-in tails (Velvet Pad's release into a large reverb,
    // Lorenz Drift) reach -80 dB about 5.8 s after the last note-off
    // (measured with vibesynth_cli --render --seconds-tail 8).
    return 6.0;
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
    job.kind = LoadJob::Kind::Builtin; // compile-only, stored trim (SPEC 9.4)
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
    // Only files get the offline matrix; built-ins and restores compile
    // with their stored trim (SPEC 9.3, 9.4).
    const bool validate = job.kind == LoadJob::Kind::Load;
    const bool isRestore = job.kind == LoadJob::Kind::Restore;
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
    else if (! isRestore)
    {
        fail (parsed.error);
        return;
    }
    else
    {
        restoreError = parsed.error;
        patch = defaultPatch;
    }

    if (validate)
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
            if (! isRestore)
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
    r.setMacros = ! isRestore;
    r.generation = job.generation;
    if (restoreError.isNotEmpty())
        r.message = "Saved patch failed (" + restoreError + "); loaded " + juce::String (patch.name);
    else
        r.message = (isRestore ? "Restored " : "Loaded ") + juce::String (patch.name)
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
    PatchProgram* program = player.program();

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
    player.setProgram (program);
    preparedRate = rate;
    preparedBlockSize = block;

    fade = Fade::None;
    fadeGain = 1.0f;
    fadeStep = 1.0f / (float) std::max (1, (int) std::lround (swapFadeSeconds * rate));

    float targets[numMacros] = {};
    for (int m = 0; m < numMacros; ++m)
        targets[m] = macroParams[(size_t) m] != nullptr ? macroParams[(size_t) m]->load() : 0.5f;
    player.prepare (rate, block, targets);
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
    if (player.program() == nullptr)
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
    // Held notes restart on their new voices (PatchPlayer::swapProgram).
    retire (player.swapProgram (incoming));
    incoming = nullptr;
    fade = Fade::In;
}

void VibeSynthAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                            juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    buffer.clear();

    if (player.maxChunk() <= 0 || numChannels <= 0)
        return;

    flushRetireStash();

    float targets[numMacros] = {};
    for (int m = 0; m < numMacros; ++m)
        targets[m] = macroParams[(size_t) m] != nullptr
                         ? macroParams[(size_t) m]->load (std::memory_order_relaxed)
                         : 0.5f;

    // bpm/beat for the patch (SPEC §4.3): host tempo, and the host's PPQ
    // position while it plays; free-running otherwise.
    {
        double bpm = 0.0, ppq = 0.0;
        bool playing = false, hasPpq = false;
        if (auto* host = getPlayHead())
        {
            if (const auto position = host->getPosition())
            {
                if (const auto hostBpm = position->getBpm())
                    bpm = *hostBpm;
                if (const auto hostPpq = position->getPpqPosition())
                {
                    ppq = *hostPpq;
                    hasPpq = true;
                }
                playing = position->getIsPlaying();
            }
        }
        player.setHostTransport (bpm, playing, hasPpq, ppq);
    }

    // Voice mode and glide (SPEC 9.1), once per block.
    {
        const int mode = voiceModeParam != nullptr
                             ? juce::jlimit (0, 2, (int) std::lround (voiceModeParam->load (std::memory_order_relaxed)))
                             : 0;
        const float glide = glideParam != nullptr ? glideParam->load (std::memory_order_relaxed) : 0.0f;
        player.setVoiceControls (static_cast<vibesynth::PatchPlayer::VoiceMode> (mode), glide);
    }

    float* outL = buffer.getWritePointer (0);
    float* outR = numChannels > 1 ? buffer.getWritePointer (1) : nullptr;

    auto midiIt = midiMessages.cbegin();
    const auto midiEnd = midiMessages.cend();

    int chunkStart = 0;
    while (chunkStart < numSamples)
    {
        pollSwapQueue();

        int n = std::min (player.maxChunk(), numSamples - chunkStart);
        // A fade-out ends exactly on a chunk boundary, where the swap runs.
        if (fade == Fade::Out)
            n = std::min (n, samplesUntilSilent());

        // Voices (sample-accurate MIDI), master VM, guard, output stage.
        player.renderChunk (n, targets, midiIt, midiEnd, chunkStart);
        const float* chunkL = player.outL();
        const float* chunkR = player.outR();

        for (int i = 0; i < n; ++i)
        {
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
            const float l = chunkL[i] * fadeGain;
            const float r = chunkR[i] * fadeGain;

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
        player.handleMidi (event.data, event.numBytes);
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
