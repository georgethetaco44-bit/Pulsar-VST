// M11 (SPEC §9.7): Mutate, "Change this node" and Undo in the processor.
// The declarations are in PluginProcessor.h (the M11 blocks); everything
// here runs on the message thread except MutateThread::run.
//
//   Change   a GenerationRequest of kind Change on the generation worker
//            (shared with Generate, newest wins). Its result is a
//            Generated load that keeps the current macro values.
//   Mutate   MutateThread runs vibesynth::Mutator (4 parallel requests,
//            one offline test at a time) and posts per-slot events; the
//            message thread folds them into mutateView. Auditioning a slot
//            is a compile-only load that pushes nothing on Undo; Keep
//            pushes the original; discarding reloads the original with
//            its exact macro values. Any other swap while the strip is
//            open closes it and pushes the original (not the audition).
//   Undo     handleAsyncUpdate calls pushUndoForSwap() before a swap is
//            shown; undo() reloads the top entry compile-only (stored
//            trim) with its exact macro values.
//
// M10 merge: the inspector's tweaks and macro maps live in the Patch
// (Patch::tweaks, macroMaps), so a snapshot's patch carries them: Undo,
// the Original card and Discard reload them exactly, and the load applies
// them to the override table (handleAsyncUpdate -> applyTweaksFrom).
// Change keeps those whose node survived (Generator); Mutate bakes them
// into the code it sends (prepareMutateBase).

#include "PluginProcessor.h"

#include "ai/NodeId.h"
#include "patch/PatchLibrary.h"

namespace
{
juce::String progressText (const vibesynth::GenerationProgress& p)
{
    using Stage = vibesynth::GenerationStage;
    juce::String s;
    switch (p.stage)
    {
        case Stage::Building: s = "Building prompt"; break;
        case Stage::Waiting:  s = "Waiting for model"; break;
        case Stage::Parsing:  s = "Parsing reply"; break;
        case Stage::Testing:  s = "Testing"; break;
        case Stage::Done:
        case Stage::Failed:
        case Stage::Cancelled: break;
    }
    if (p.retry > 0)
        s << " (retry " << p.retry << "/" << vibesynth::Generator::kMaxRetries << ")";
    return s;
}
} // namespace

//==============================================================================
// Snapshots (Undo, Mutate's original).

vibesynth::PatchSnapshot VibeSynthAudioProcessor::captureSnapshot() const
{
    vibesynth::PatchSnapshot s;
    s.patch = uiPatch;
    for (int i = 0; i < numMacros; ++i)
        s.macros[(size_t) i] = macroParams[(size_t) i]->load();
    s.source = activeSource.toStdString();
    s.program = currentProgramIndex.load();
    s.nodes = uiNodes;
    s.nodesKnown = nodesKnown;
    return s;  // s.patch includes the tweaks and macro maps
}

void VibeSynthAudioProcessor::applySnapshot (const vibesynth::PatchSnapshot& s, const juce::String& verb)
{
    LoadJob job;
    job.kind = LoadJob::Kind::Generated;   // compile only, with the stored trim
    job.text = vibesynth::serializePatch (s.patch);
    job.source = juce::String (s.patch.name);
    job.sourceKey = juce::String (s.source);
    job.hasNodes = s.nodesKnown;
    job.nodes = s.nodes;
    job.pushUndo = false;
    job.hasMacroValues = true;
    job.macroValues = s.macros;
    job.verb = verb;
    if (s.program >= 0 && s.program < getNumPrograms())
        currentProgramIndex = s.program;
    postJob (std::move (job));  // the text carries the tweaks and macro maps
}

void VibeSynthAudioProcessor::pushUndoForSwap()
{
    if (mutateView.open)
    {
        // A preset, generation or change replaced an audition: Undo goes
        // back to what played before Mutate, and the strip closes.
        undoStack.push (mutateOriginal);
        closeMutateSession();
    }
    else
    {
        undoStack.push (captureSnapshot());
    }
    ++editVersion;
}

void VibeSynthAudioProcessor::undo()
{
    if (mutateView.open)
    {
        discardMutation();
        return;
    }
    vibesynth::PatchSnapshot s;
    if (! undoStack.pop (s))
        return;
    applySnapshot (s, "Undo: back to");
    ++editVersion;
    setStatus ("Undo: back to '" + juce::String (s.patch.name) + "'", StatusKind::Busy);
}

//==============================================================================
// Change this node.

bool VibeSynthAudioProcessor::requestNodeChange (const juce::String& nodeId, const juce::String& instruction)
{
    const auto text = instruction.trim();
    if (text.isEmpty())
    {
        setStatus ("Say what to change", StatusKind::Error);
        return false;
    }
    auto& s = aiSettings();
    if (! s.hasKey())
    {
        setStatus ("Change this node needs an API key: add one in Settings (or set VIBESYNTH_API_KEY)",
                   StatusKind::Error);
        return false;
    }
    vibesynth::NodeRef node;
    std::string error;
    if (! vibesynth::parseNodeId (nodeId.toStdString(), node, error))
    {
        setStatus (juce::String (error), StatusKind::Error);
        return false;
    }
    const juce::String id (vibesynth::nodeIdString (node));
    if (nodesKnown && ! vibesynth::nodeIsTouched (uiNodes, node))
    {
        setStatus ("Node " + id + " isn't in this patch", StatusKind::Error);
        return false;
    }

    auto request = std::make_unique<vibesynth::GenerationRequest>();
    request->kind = vibesynth::GenerationKind::Change;
    request->base = uiPatch;
    request->baseNodes = uiNodes;
    request->baseNodesKnown = nodesKnown;
    request->node = id.toStdString();
    request->instruction = text.toStdString();
    request->config = s.config();
    request->apiKey = s.apiKey();
    const double rate = preparedRate.load();
    request->sampleRate = rate > 0.0 ? rate : 48000.0;
    {
        std::lock_guard<std::mutex> lock (generationMutex);
        nextGeneration = std::move (request);
        generationPending = true;
        cancelGenerationFlag = true;
        changeRunning = true;
    }
    generationWorker.notify();
    generationStartMs = juce::Time::getMillisecondCounterHiRes();
    generationStage = "Changing " + id;
    setStatus ("Changing " + id, StatusKind::Busy);
    return true;
}

//==============================================================================
// Mutate.

bool VibeSynthAudioProcessor::canMutate()
{
    const bool running = mutateView.running && mutateThread != nullptr && mutateThread->isThreadRunning();
    return aiSettings().hasKey() && ! running;
}

void VibeSynthAudioProcessor::startMutate()
{
    auto& settingsRef = aiSettings();
    if (! settingsRef.hasKey())
    {
        setStatus ("Mutate needs an API key: add one in Settings (or set VIBESYNTH_API_KEY)", StatusKind::Error);
        return;
    }
    if (! canMutate())
        return;
    // Mutating while a variation plays builds on that variation.
    if (mutateView.open && mutateView.auditioning >= 0)
        keepMutation();
    else if (mutateView.open)
        closeMutateSession();
    if (mutateThread != nullptr)
    {
        // Finished or cancelled: it ends within a request poll.
        mutateThread->cancel = true;
        mutateThread->stopThread (3000);
        mutateThread.reset();
    }

    mutateOriginal = captureSnapshot();
    ++mutateSession;
    mutateResults = {};
    mutateView = {};
    mutateView.open = true;
    mutateView.running = true;
    mutateView.baseName = uiPatch.name;
    mutateView.startedMs = juce::Time::getMillisecondCounterHiRes();
    const auto& dirs = vibesynth::PromptResources::embedded().mutateDirections;
    for (int i = 0; i < vibesynth::MutateView::kSlots; ++i)
    {
        auto& v = mutateView.slots[(size_t) i];
        v.state = vibesynth::MutateSlotView::State::Waiting;
        v.direction = i < (int) dirs.size() ? dirs[(size_t) i].label : "Variation " + std::to_string (i + 1);
        v.text = "Waiting for model";
    }

    // M10 merge (SPEC §9.6): the tweaks are baked into the code sent, so the
    // variations keep the user's shaping (and start with no tweaks).
    const auto base = vibesynth::prepareMutateBase (uiPatch);
    mutateNote = juce::String (base.note);
    vibesynth::MutateRequest req;
    req.base = base.patch;
    req.baseNodes = uiNodes;
    req.baseNodesKnown = nodesKnown && ! base.baked;
    req.config = settingsRef.config();
    req.apiKey = settingsRef.apiKey();
    const double rate = preparedRate.load();
    req.sampleRate = rate > 0.0 ? rate : 48000.0;
    mutateThread = std::make_unique<MutateThread> (*this, std::move (req), mutateSession);
    mutateThread->startThread();
    ++editVersion;
    setStatus ("Mutating '" + juce::String (uiPatch.name) + "': 4 variations on their way"
                   + (mutateNote.isNotEmpty() ? " (without your tweaks)" : ""),
               StatusKind::Info, mutateNote);
}

void VibeSynthAudioProcessor::MutateThread::run()
{
    // Only stage or retry changes are posted (the strip shows its own clock).
    std::array<int, vibesynth::MutateView::kSlots> lastStage {}, lastRetry {};
    lastStage.fill (-1);
    vibesynth::Mutator::run (
        request, &cancel,
        [this, &lastStage, &lastRetry] (int slot, const vibesynth::GenerationProgress& p)
        {
            using Stage = vibesynth::GenerationStage;
            if (slot < 0 || slot >= vibesynth::MutateView::kSlots || p.stage == Stage::Done
                || p.stage == Stage::Failed || p.stage == Stage::Cancelled)
                return;
            if (lastStage[(size_t) slot] == (int) p.stage && lastRetry[(size_t) slot] == p.retry)
                return;
            lastStage[(size_t) slot] = (int) p.stage;
            lastRetry[(size_t) slot] = p.retry;
            MutateEvent e;
            e.session = session;
            e.slot = slot;
            e.progress = p;
            owner.postMutateEvent (std::move (e));
        },
        [this] (int slot, const vibesynth::GenerationResult& r)
        {
            MutateEvent e;
            e.session = session;
            e.slot = slot;
            e.done = true;
            e.result = std::make_shared<vibesynth::GenerationResult> (r);
            owner.postMutateEvent (std::move (e));
        });
    MutateEvent finished;
    finished.session = session;
    finished.done = true;
    owner.postMutateEvent (std::move (finished));
}

void VibeSynthAudioProcessor::postMutateEvent (MutateEvent e)
{
    {
        std::lock_guard<std::mutex> lock (resultMutex);
        mutateEvents.push_back (std::move (e));
    }
    triggerAsyncUpdate();
}

void VibeSynthAudioProcessor::applyMutateEvents()
{
    std::vector<MutateEvent> events;
    {
        std::lock_guard<std::mutex> lock (resultMutex);
        events.swap (mutateEvents);
    }
    bool changed = false, finished = false;
    using State = vibesynth::MutateSlotView::State;
    for (auto& e : events)
    {
        if (e.session != mutateSession || ! mutateView.open)
            continue;
        changed = true;
        if (e.slot < 0)
        {
            mutateView.running = false;
            finished = true;
            continue;
        }
        if (e.slot >= vibesynth::MutateView::kSlots)
            continue;
        auto& v = mutateView.slots[(size_t) e.slot];
        if (! e.done)
        {
            if (v.state == State::Waiting || v.state == State::Testing)
            {
                v.state = e.progress.stage == vibesynth::GenerationStage::Testing ? State::Testing : State::Waiting;
                v.text = progressText (e.progress).toStdString();
            }
            continue;
        }
        const auto& r = *e.result;
        v.detail.clear();
        if (r.ok)
        {
            v.state = State::Ready;
            v.name = r.patch.name;
            v.text = "Ready";
            mutateResults[(size_t) e.slot] = e.result;
        }
        else if (r.cancelled)
        {
            v.state = State::Cancelled;
            v.text = "Cancelled";
        }
        else
        {
            v.state = State::Failed;
            v.text = juce::String (r.reason).upToFirstOccurrenceOf ("\n", false, false).toStdString();
            v.detail = r.reason;
        }
    }
    if (! changed)
        return;
    ++editVersion;
    if (finished)
    {
        int ready = 0;
        for (const auto& v : mutateView.slots)
            ready += v.state == State::Ready ? 1 : 0;
        if (ready > 0)
            status = { StatusKind::Success,
                       juce::String (ready) + " of 4 variations ready: click one to hear it, Keep to use it",
                       mutateNote };
        else if (mutateView.slots[0].state == State::Cancelled)
            status = { StatusKind::Info, "Mutate cancelled", {} };
        else
            status = { StatusKind::Error, "Mutate failed: " + juce::String (mutateView.slots[0].text),
                       juce::String (mutateView.slots[0].detail) + "\n\nThe current patch is still playing."
                           + (mutateNote.isNotEmpty() ? "\n\n" + mutateNote : juce::String()) };
    }
    sendChangeMessage();
}

void VibeSynthAudioProcessor::cancelMutate()
{
    if (mutateThread != nullptr)
        mutateThread->cancel = true;
}

void VibeSynthAudioProcessor::auditionMutation (int slot)
{
    if (! mutateView.open || slot == mutateView.auditioning || slot >= vibesynth::MutateView::kSlots)
        return;
    if (slot < 0)
    {
        applySnapshot (mutateOriginal, "Original:");
    }
    else
    {
        const auto r = mutateResults[(size_t) slot];
        if (r == nullptr)
            return;
        LoadJob job;
        job.kind = LoadJob::Kind::Generated;  // tested by the Mutator; compile only
        job.text = vibesynth::serializePatch (r->patch);
        job.source = juce::String (r->patch.name);
        job.sourceKey = "mutate:" + juce::String (slot);
        job.hasNodes = true;
        job.nodes = r->touched;
        job.pushUndo = false;
        job.verb = "Auditioning " + juce::String (mutateView.slots[(size_t) slot].direction) + ":";
        postJob (std::move (job));
    }
    mutateView.auditioning = slot;
    ++editVersion;
    sendChangeMessage();
}

void VibeSynthAudioProcessor::keepMutation()
{
    if (! mutateView.open || mutateView.auditioning < 0)
        return;
    const auto r = mutateResults[(size_t) mutateView.auditioning];
    if (r == nullptr)
        return;
    undoStack.push (mutateOriginal);
    juce::String historyError;
    const auto saved = vibesynth::PatchLibrary::addToHistory (vibesynth::PatchLibrary::historyFolder(), r->patch,
                                                              juce::Time::getCurrentTime(), historyError);
    if (saved != juce::File())
    {
        ++historyVersion;
        activeSource = saved.getFullPathName();
    }
    closeMutateSession();
    setStatus ("Kept '" + juce::String (r->patch.name) + "'", StatusKind::Success,
               historyError.isNotEmpty() ? "Not saved to the history: " + historyError : juce::String());
}

void VibeSynthAudioProcessor::discardMutation()
{
    if (! mutateView.open)
        return;
    const bool reload = mutateView.auditioning >= 0;
    const auto original = mutateOriginal;
    closeMutateSession();
    if (reload)
        applySnapshot (original, "Back to");
    else
        setStatus ("Variations discarded; '" + juce::String (original.patch.name) + "' is still playing");
}

void VibeSynthAudioProcessor::closeMutateSession()
{
    if (mutateThread != nullptr)
        mutateThread->cancel = true;
    mutateView.open = false;
    mutateView.running = false;  // its thread may take a moment to end; startMutate joins it
    mutateView.auditioning = -1;
    mutateResults = {};
    ++mutateSession;  // late events are ignored
    ++editVersion;
    sendChangeMessage();
}

void VibeSynthAudioProcessor::stopEditThreads()
{
    if (mutateThread != nullptr)
    {
        mutateThread->cancel = true;
        mutateThread->stopThread (10000);
        mutateThread.reset();
    }
}
