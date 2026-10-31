#include "PluginProcessor.h"

#include "PluginEditor.h"

#include <algorithm>

#include "patch/PatchLibrary.h"

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
// M8: a generation worker runs vibesynth::Generator (network + offline
// tests) and posts a passing patch to the load worker as a Generated job;
// its progress text reaches the status line through the AsyncUpdater.
//
// M9: results carry the node list (validator) or, for compile-only loads,
// ask the load worker for a probe render when it is idle; the audio
// thread publishes rack and output peaks into atomics once per block.
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
    status.text = "Loaded " + juce::String (defaultPatch.name);
    uiPatch = defaultPatch;
    probeRequested = true; // the default patch's nodes, once the worker runs

    player.setNodeHooks (overrideTable.get(), traceBank.get());  // M10

    worker.startThread();
    generationWorker.startThread(); // idles until generate(); no network here
}

VibeSynthAudioProcessor::~VibeSynthAudioProcessor()
{
    // Stop the generation worker first (a request in flight is cancelled
    // within about a second, a validation after its current sample), then
    // the load worker it posts to, then free everything they or the audio
    // thread owned.
    if (keyTestThread != nullptr)
    {
        keyTestThread->cancel = true;
        keyTestThread->stopThread (10000);
    }
    stopEditThreads(); // M11: the Mutate thread
    cancelGenerationFlag = true;
    generationWorker.signalThreadShouldExit();
    generationWorker.notify();
    generationWorker.stopThread (10000);
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
    job.sourceKey = "builtin:" + juce::String (index);
    postJob (std::move (job));
}

void VibeSynthAudioProcessor::loadPatchFile (const juce::File& file)
{
    // Refuse oversize files before reading them; parsePatch re-checks.
    if (file.getSize() > (juce::int64) vibesynth::Patch::kMaxFileBytes)
    {
        setStatus ("Rejected " + file.getFileName() + ": patch file too large (max 256 KB)",
                   StatusKind::Error);
        return;
    }
    LoadJob job;
    job.kind = LoadJob::Kind::Load;
    job.text = file.loadFileAsString().toStdString();
    job.source = file.getFileNameWithoutExtension();
    job.sourceKey = file.getFullPathName();
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

void VibeSynthAudioProcessor::setStatus (const juce::String& text, StatusKind kind, const juce::String& detail)
{
    status = { kind, text, detail };
    sendChangeMessage();
}

void VibeSynthAudioProcessor::adoptSavedPatch (const juce::File& file, const vibesynth::Patch& patch)
{
    {
        std::lock_guard<std::mutex> lock (patchMutex);
        if (activePatch.voice == patch.voice && activePatch.init == patch.init && activePatch.master == patch.master)
            activePatch = patch;
    }
    uiPatch = patch;
    ++patchVersion;
    activeSource = file.getFullPathName();
    setStatus ("Saved '" + juce::String (patch.name) + "' to your patches", StatusKind::Info, file.getFullPathName());
}

double VibeSynthAudioProcessor::getGenerationSeconds() const
{
    return isGenerating() ? (juce::Time::getMillisecondCounterHiRes() - generationStartMs) / 1000.0 : 0.0;
}

//==============================================================================
// M8 generation (SPEC 6). Message thread: generate / cancel / settings.

vibesynth::AiSettings& VibeSynthAudioProcessor::aiSettings()
{
    if (settings == nullptr)
        settings = std::make_unique<vibesynth::AiSettings>();
    return *settings;
}

void VibeSynthAudioProcessor::generate (const juce::String& prompt)
{
    const auto text = prompt.trim();
    if (text.isEmpty())
    {
        setStatus ("Type a prompt first", StatusKind::Error);
        return;
    }
    auto& s = aiSettings();
    if (! s.hasKey())
    {
        setStatus ("Generate needs an API key: add one in Settings (or set VIBESYNTH_API_KEY)",
                   StatusKind::Error);
        return;
    }
    auto request = std::make_unique<vibesynth::GenerationRequest>();
    request->prompt = text.toStdString();
    request->config = s.config();
    request->apiKey = s.apiKey();
    const double rate = preparedRate.load();
    request->sampleRate = rate > 0.0 ? rate : 48000.0;
    {
        // A generation in flight is now stale: cancel it, newest wins.
        std::lock_guard<std::mutex> lock (generationMutex);
        nextGeneration = std::move (request);
        generationPending = true;
        cancelGenerationFlag = true;
        changeRunning = false; // M11: a pending change is superseded
    }
    generationWorker.notify();
    generationStartMs = juce::Time::getMillisecondCounterHiRes();
    generationStage = "Starting";
    setStatus ("Generating: starting", StatusKind::Busy);
}

void VibeSynthAudioProcessor::testApiKey (const std::string& key, const vibesynth::AiConfig& config)
{
    if (keyTestThread != nullptr)
    {
        keyTestThread->cancel = true;   // the request polls this every 100 ms
        keyTestThread->stopThread (5000);
        keyTestThread.reset();
    }
    const int id = ++keyTestId;
    if (juce::String (key).trim().isEmpty())
    {
        keyTest = { KeyTest::State::Failed, "Enter a key first (or set VIBESYNTH_API_KEY)" };
        sendChangeMessage();
        return;
    }
    keyTest = { KeyTest::State::Running, "Testing the key..." };
    keyTestThread = std::make_unique<KeyTestThread> (*this, key, config, id);
    keyTestThread->startThread();
    sendChangeMessage();
}

void VibeSynthAudioProcessor::clearKeyTest()
{
    ++keyTestId;   // a reply still on its way is stale
    keyTest = {};
}

// Key test thread: one request, then the result goes to the message thread.
void VibeSynthAudioProcessor::KeyTestThread::run()
{
    const auto reply = vibesynth::testApiKey (config, key, &cancel);
    if (cancel.load() || reply.error == vibesynth::ChatError::Cancelled)
        return;
    const bool ok = vibesynth::keyTestPassed (reply);
    owner.postKeyTest (id, { ok ? KeyTest::State::Ok : KeyTest::State::Failed,
                             juce::String (vibesynth::keyTestMessage (reply)) });
}

void VibeSynthAudioProcessor::postKeyTest (int id, const KeyTest& result)
{
    {
        std::lock_guard<std::mutex> lock (resultMutex);
        postedKeyTest = result;
        postedKeyTestId = id;
        keyTestPosted = true;
    }
    triggerAsyncUpdate();
}

void VibeSynthAudioProcessor::cancelGeneration()
{
    {
        std::lock_guard<std::mutex> lock (generationMutex);
        nextGeneration.reset();
        generationPending = false;
        cancelGenerationFlag = true;
    }
    if (! generating.load())
    {
        setStatus ("Generation cancelled");
    }
    else
    {
        generationStage = "Cancelling";
        setStatus ("Cancelling", StatusKind::Busy);
    }
}

void VibeSynthAudioProcessor::GenerationWorker::run()
{
    while (! threadShouldExit())
    {
        std::unique_ptr<vibesynth::GenerationRequest> request;
        {
            std::lock_guard<std::mutex> lock (owner.generationMutex);
            if (owner.nextGeneration != nullptr)
            {
                request = std::move (owner.nextGeneration);
                owner.generating = true;
                owner.generationPending = false;
                owner.cancelGenerationFlag = false;
                owner.changeRunning = request->kind == vibesynth::GenerationKind::Change;
            }
        }
        if (request != nullptr)
        {
            owner.runGeneration (*request);
            owner.changeRunning = false;
        }
        else
            wait (100);
    }
}

// Generation worker thread.
void VibeSynthAudioProcessor::runGeneration (const vibesynth::GenerationRequest& request)
{
    juce::String last;
    auto onProgress = [this, &last] (const vibesynth::GenerationProgress& p)
    {
        using Stage = vibesynth::GenerationStage;
        if (p.stage == Stage::Done || p.stage == Stage::Failed || p.stage == Stage::Cancelled)
            return; // the final status comes below
        // The editor shows its own elapsed time, so the wait seconds are
        // left out (fewer, steadier updates).
        auto q = p;
        q.waitSeconds = 0.0;
        auto s = juce::String (vibesynth::describeProgress (q));
        if (p.stage == Stage::Waiting)
            s = s.upToFirstOccurrenceOf (" (0 s)", false, false)
              + s.fromFirstOccurrenceOf (" (0 s)", false, false);
        if (s != last)
        {
            last = s;
            postGenerationStatus (StatusKind::Busy, s);
        }
    };
    const auto res = vibesynth::Generator::run (request, &cancelGenerationFlag, onProgress);
    generating = false;
    const bool isChange = request.kind == vibesynth::GenerationKind::Change;  // M11

    if (res.cancelled || generationPending.load())
    {
        // Superseded by a newer prompt (which reports itself), or cancelled.
        if (! generationPending.load())
            postGenerationStatus (StatusKind::Info, isChange ? "Change cancelled" : "Generation cancelled");
        return;
    }
    if (! res.ok)
    {
        const juce::String reason (res.reason);
        postGenerationStatus (StatusKind::Error, (isChange ? "Change failed: " : "Generation failed: ") + reason,
                              reason + "\n\nThe current patch is still playing.");
        return;
    }

    // M9 (SPEC §9.5): every successful generation goes to the history.
    juce::String historyError;
    const auto saved = vibesynth::PatchLibrary::addToHistory (vibesynth::PatchLibrary::historyFolder(), res.patch,
                                                              juce::Time::getCurrentTime(), historyError);
    if (saved != juce::File())
        ++historyVersion;

    const int attempts = (int) res.attempts.size();
    LoadJob job;
    job.kind = LoadJob::Kind::Generated;
    job.text = vibesynth::serializePatch (res.patch);
    job.source = juce::String (res.patch.name);
    job.sourceKey = saved.getFullPathName();
    job.hasNodes = true;
    job.nodes = res.touched;
    juce::String note = juce::String (attempts) + (attempts == 1 ? " attempt, " : " attempts, ")
                      + juce::String (res.seconds, 1) + " s";
    if (! res.reason.empty())
        note << "\n" << juce::String (res.reason);
    if (historyError.isNotEmpty())
        note << "\nNot saved to the history: " << historyError;
    if (isChange)
    {
        // M11: an edit keeps the user's macro positions; say what it dropped.
        job.keepMacros = true;
        job.verb = "Changed " + juce::String (request.node) + ":";
        if (! res.lostNodes.empty())
            note << "\nNo longer in the patch: " << juce::String (res.lostNodes);
        for (const auto& l : res.labelChanges)
            note << "\nMacro relabelled: " << juce::String (l);
        // M10 merge: tweaks on surviving nodes are kept (res.patch has them).
        for (const auto& t : res.droppedTweaks)
            note << "\nTweak dropped (its node is gone): " << juce::String (t);
    }
    job.detail = note;
    postJob (std::move (job));
}

void VibeSynthAudioProcessor::postGenerationStatus (StatusKind kind, const juce::String& text,
                                                    const juce::String& detail)
{
    {
        std::lock_guard<std::mutex> lock (resultMutex);
        generationStatus = { kind, text, detail };
        generationStatusPending = true;
    }
    triggerAsyncUpdate();
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
        else if (owner.nodesWanted.load() && owner.probeRequested.exchange (false))
            owner.probeActivePatch();
        else
            wait (20);
    }
}

// Load worker, idle: the node list of whatever is (about to be) playing
// (SPEC §9.6). A newer job cancels it; the message thread drops a result
// whose code no longer matches the patch it shows.
void VibeSynthAudioProcessor::probeActivePatch()
{
    vibesynth::Patch patch;
    {
        std::lock_guard<std::mutex> lock (patchMutex);
        patch = activePatch;
    }
    const double rate = preparedRate.load();
    LoadResult r;
    r.nodes = vibesynth::probeTouchedNodes (patch, rate > 0.0 ? rate : 48000.0, &cancelValidation);
    if (cancelValidation.load())
    {
        probeRequested = true; // try again after the new job
        return;
    }
    r.ok = true;
    r.nodesOnly = true;
    r.hasNodes = true;
    r.patch = std::move (patch);
    postResult (std::move (r));
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
    // Only files get the offline matrix here; built-ins and restores
    // compile with their stored trim (SPEC 9.3, 9.4), and generated
    // patches were tested on the generation worker (trim set).
    const bool validate = job.kind == LoadJob::Kind::Load;
    const bool isRestore = job.kind == LoadJob::Kind::Restore;
    const bool isGenerated = job.kind == LoadJob::Kind::Generated;
    auto stale = [this, &job] { return job.generation != generation.load(); };
    auto fail = [this, &job] (const juce::String& reason)
    {
        LoadResult r;
        r.kind = StatusKind::Error;
        r.message = "Rejected " + job.source + ": " + reason;
        r.detail = reason;
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

    bool hasNodes = job.hasNodes;
    vibesynth::TouchedNodes nodes = job.nodes;
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
        hasNodes = true;
        nodes = vr.touched;
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
            hasNodes = false;
            if (! buildAndPushLocked (patch, error))
                return;
        }
        std::lock_guard<std::mutex> plock (patchMutex);
        activePatch = patch;
    }

    LoadResult r;
    r.ok = true;
    r.patch = patch;
    r.setMacros = ! isRestore && ! job.keepMacros && ! job.hasMacroValues;
    r.pushUndo = job.pushUndo && ! isRestore;      // M11
    r.hasMacroValues = job.hasMacroValues;
    r.macroValues = job.macroValues;
    r.generation = job.generation;
    r.sourceKey = restoreError.isNotEmpty() ? juce::String ("builtin:0") : job.sourceKey;
    r.hasNodes = hasNodes;
    r.nodes = nodes;
    const juce::String dot = juce::String::fromUTF8 (" \xc2\xb7 ");
    if (restoreError.isNotEmpty())
    {
        r.kind = StatusKind::Error;
        r.message = "Saved patch failed; loaded " + juce::String (patch.name);
        r.detail = restoreError;
    }
    else if (isGenerated)
    {
        r.kind = StatusKind::Success;
        r.message = (job.verb.isNotEmpty() ? job.verb : juce::String ("Generated")) + " '" + juce::String (patch.name)
                  + "'" + dot + "trim " + formatDb (patch.trim);
        r.detail = job.detail;
    }
    else
    {
        r.message = (isRestore ? "Restored " : "Loaded ") + juce::String (patch.name) + dot + "trim "
                  + formatDb (patch.trim);
    }
    postResult (std::move (r));
    // Compile-only loads (built-ins, restores) learn their nodes from a
    // probe render once the worker is idle.
    if (! hasNodes)
        probeRequested = true;
}

void VibeSynthAudioProcessor::postResult (LoadResult result)
{
    {
        std::lock_guard<std::mutex> lock (resultMutex);
        results.push_back (std::move (result));
    }
    triggerAsyncUpdate();
}

// Message thread: macro defaults (SPEC 9.2), labels, status line, and the
// editor's patch, source and node list (M9).
void VibeSynthAudioProcessor::handleAsyncUpdate()
{
    std::vector<LoadResult> done;
    Status genStatus;
    bool genStatusChanged = false;
    bool keyTestChanged = false;
    {
        std::lock_guard<std::mutex> lock (resultMutex);
        done.swap (results);
        if (keyTestPosted)
        {
            keyTestPosted = false;
            if (postedKeyTestId == keyTestId)
            {
                keyTest = postedKeyTest;
                keyTestChanged = true;
            }
        }
        genStatusChanged = generationStatusPending;
        genStatus = generationStatus;
        generationStatusPending = false;
    }
    applyMutateEvents(); // M11: audition strip progress and results

    if (genStatusChanged)
    {
        if (genStatus.kind == StatusKind::Busy)
        {
            generationStage = genStatus.text;
            status = { StatusKind::Busy, "Generating: " + genStatus.text, {} };
        }
        else
        {
            status = genStatus;
        }
    }

    bool labelsChanged = false, anyLoad = false;
    for (auto& r : done)
    {
        if (r.nodesOnly)
        {
            // A probe for the patch the UI shows (not one replaced since).
            const bool same = r.patch.init == uiPatch.init && r.patch.voice == uiPatch.voice
                           && r.patch.master == uiPatch.master;
            if (same && (! nodesKnown || r.nodes != uiNodes))
            {
                uiNodes = r.nodes;
                nodesKnown = true;
                ++nodesVersion;
                anyLoad = true;
            }
            continue;
        }
        status = { r.kind, r.message, r.detail };
        anyLoad = true;
        if (! r.ok)
        {
            // The old patch keeps playing; make sure its nodes arrive.
            if (! nodesKnown)
            {
                probeRequested = true;
                worker.notify();
            }
            continue;
        }
        if (r.pushUndo)
            pushUndoForSwap(); // M11: before uiPatch, the macros and the source change
        uiPatch = r.patch;
        applyTweaksFrom (r.patch);  // M10: the new patch's tweaks (none clears them)
        ++patchVersion;
        if (r.sourceKey.isNotEmpty())
            activeSource = r.sourceKey;
        uiNodes = r.hasNodes ? r.nodes : vibesynth::TouchedNodes();
        nodesKnown = r.hasNodes;
        ++nodesVersion;
        for (int i = 0; i < numMacros; ++i)
        {
            const juce::String label (i < (int) r.patch.macros.size() ? r.patch.macros[(size_t) i].label
                                                                     : std::string());
            labelsChanged = labelsChanged || label != macroLabels[(size_t) i];
            macroLabels[(size_t) i] = label;
        }
        // Only the newest load sets macros; a later restore or load wins.
        if (r.setMacros && r.generation == generation.load())
            for (int i = 0; i < numMacros; ++i)
                if (auto* param = apvts.getParameter ("macro" + juce::String (i + 1)))
                    param->setValueNotifyingHost ((float) r.patch.macroDefault (i));
        // M11: Undo and discard restore the exact values they captured.
        if (r.hasMacroValues && r.generation == generation.load())
            for (int i = 0; i < numMacros; ++i)
                if (auto* param = apvts.getParameter ("macro" + juce::String (i + 1)))
                    param->setValueNotifyingHost (r.macroValues[(size_t) i]);
    }
    if (anyLoad)
    {
        // SPEC 9.2: hosts that re-read parameter info may show the labels.
        auto details = ChangeDetails().withProgramChanged (true);
        if (labelsChanged)
            details = details.withParameterInfoChanged (true);
        updateHostDisplay (details);
        sendChangeMessage();
    }
    else if (genStatusChanged || keyTestChanged)
    {
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
            r.kind = StatusKind::Error;
            r.message = "Saved patch failed; loaded " + juce::String (defaultPatch.name);
            r.detail = reason;
            r.sourceKey = "builtin:0";
            r.generation = generation.load();
            postResult (std::move (r));
            probeRequested = true;
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

    // M9 meters for the editor (relaxed atomics, no allocation).
    player.harvestMeters (meterBank);
    publishOutputPeak (0, buffer.getMagnitude (0, 0, numSamples));
    publishOutputPeak (1, buffer.getMagnitude (numChannels > 1 ? 1 : 0, 0, numSamples));
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
    state.setProperty ("uiScale", (double) editorScale.load(), nullptr);

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
    const double uiScale = tree.getProperty ("uiScale", 0.0);
    tree.removeProperty ("patchJson", nullptr);
    tree.removeProperty ("program", nullptr);
    tree.removeProperty ("uiScale", nullptr);
    if (uiScale >= 0.75 && uiScale <= 1.5)
        editorScale = (float) uiScale;
    apvts.replaceState (tree);

    if (programIndex >= 0 && programIndex < getNumPrograms())
        currentProgramIndex = programIndex;
    if (json.isEmpty())
        return; // an older state without a patch: keep the current one

    // Compile on the worker (or in prepareToPlay if not prepared yet),
    // never here: this may run on any thread pluginval picks.
    const auto parsed = vibesynth::parsePatch (json.toStdString());
    // The editor highlights the built-in only if that is what was saved.
    juce::String sourceKey ("restored");
    if (parsed.ok() && programIndex >= 0 && programIndex < getNumPrograms())
    {
        const auto builtin = vibesynth::parsePatch (vibesynth::builtinPresetText (programIndex));
        if (builtin.ok() && builtin.patch.voice == parsed.patch.voice && builtin.patch.init == parsed.patch.init
            && builtin.patch.master == parsed.patch.master)
            sourceKey = "builtin:" + juce::String (programIndex);
    }
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
                      + juce::String::fromUTF8 (" \xc2\xb7 trim ") + formatDb (parsed.patch.trim);
            r.sourceKey = sourceKey;
            r.generation = generation.load();
            postResult (std::move (r));
            probeRequested = true;
            return;
        }
    }

    LoadJob job;
    job.kind = LoadJob::Kind::Restore;
    job.text = json.toStdString();
    job.source = "saved patch";
    job.sourceKey = sourceKey;
    postJob (std::move (job));
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VibeSynthAudioProcessor();
}
