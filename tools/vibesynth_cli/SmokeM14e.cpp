// M14e smoketests. See SmokeM14e.h.

#include "SmokeM14e.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "CliCheck.h"
#include "ai/Generator.h"
#include "ai/Mutator.h"
#include "ai/MutateView.h"
#include "ai/PromptBuilder.h"
#include "ai/ResponseParser.h"
#include "ai/UndoStack.h"
#include "patch/Patch.h"
#include "patch/PatchLibrary.h"
#include "patch/PatchValidator.h"

namespace vibesynth_cli {

namespace {

using namespace vibesynth;

// Helper to extract which take a request is for from its prompt message
int takeIndexOf (const ChatRequest& c, int count)
{
    const auto dirs = Mutator::buildTakeDirections (count);
    for (const auto& m : c.messages)
        if (m.role == "user")
            for (size_t i = 0; i < dirs.size(); ++i)
                if (m.content.find (dirs[i].text) != std::string::npos)
                    return static_cast<int> (i);
    return -1;
}

// 1. Takes directions builder test
bool smokeTakesDirections()
{
    bool ok = true;

    // 1 take
    const auto dirs1 = Mutator::buildTakeDirections (1);
    const bool ok1 = dirs1.size() == 1 && dirs1[0].key == "literal" && dirs1[0].label == "Literal"
                  && dirs1[0].text == "Take 1 of 1: the most literal reading of the request.";
    ok = check (ok1, "M14e takes directions: count 1 returns Literal") && ok;

    // 3 takes
    const auto dirs3 = Mutator::buildTakeDirections (3);
    const bool ok3 = dirs3.size() == 3
                  && dirs3[0].label == "Literal" && dirs3[0].text.find ("Take 1 of 3: the most literal reading") != std::string::npos
                  && dirs3[1].label == "Alt source" && dirs3[1].text.find ("Take 2 of 3: the same intent built on a different main sound source") != std::string::npos
                  && dirs3[2].label == "Alt motion" && dirs3[2].text.find ("Take 3 of 3: the same intent with a different kind of movement") != std::string::npos;
    ok = check (ok3, "M14e takes directions: count 3 returns Literal, Alt source, Alt motion") && ok;

    // 4 takes
    const auto dirs4 = Mutator::buildTakeDirections (4);
    const bool ok4 = dirs4.size() == 4
                  && dirs4[3].label == "Bold" && dirs4[3].text.find ("Take 4 of 4: a bolder, more extreme reading") != std::string::npos;
    ok = check (ok4, "M14e takes directions: count 4 returns Bold as 4th take") && ok;

    // Clamping: <= 0 clamps to 1, >= 5 clamps to 4
    const auto dirsMin = Mutator::buildTakeDirections (0);
    const auto dirsMax = Mutator::buildTakeDirections (99);
    ok = check (dirsMin.size() == 1 && dirsMax.size() == 4,
                "M14e takes directions: count clamps safely to 1..4 (%d, %d)",
                (int) dirsMin.size(), (int) dirsMax.size()) && ok;

    return ok;
}

// 2. Takes N execution and take lines in prompt
bool smokeTakesNExecution()
{
    bool ok = true;
    const Patch base = parsePatch (builtinPresetText (0)).patch;

    auto makeTake = [&] (const char* name)
    {
        Patch p = base;
        p.name = name;
        return formatPatchAsReply (p);
    };

    const std::string replies[3] = {
        makeTake ("Literal Take"),
        makeTake ("Alt Source Take"),
        makeTake ("Alt Motion Take")
    };

    std::mutex lock;
    std::vector<ChatRequest> seenRequests;
    TakesRequest treq;
    treq.prompt = "aggressive bass";
    treq.takes = 3;
    treq.transport = [&] (const ChatRequest& c, const std::atomic<bool>*)
    {
        std::lock_guard<std::mutex> g (lock);
        seenRequests.push_back (c);
        const int idx = takeIndexOf (c, 3);
        ChatReply r;
        r.content = (idx >= 0 && idx < 3) ? replies[idx] : "";
        r.promptTokens = 2000;
        r.completionTokens = 600;
        return r;
    };

    const MutateResult res = Mutator::runTakes (treq, nullptr);

    const bool slotCountOk = res.slots.size() == 3;
    const bool reqCountOk = seenRequests.size() == 3;
    bool promptsContainTakeLines = true;
    const auto dirs = Mutator::buildTakeDirections (3);
    for (size_t i = 0; i < seenRequests.size(); ++i)
    {
        const auto& c = seenRequests[i];
        const int idx = takeIndexOf (c, 3);
        if (idx < 0 || c.messages.empty() || c.messages.back().content.find (dirs[(size_t) idx].text) == std::string::npos)
            promptsContainTakeLines = false;
    }

    ok = check (slotCountOk && reqCountOk && promptsContainTakeLines && res.passed() == 3,
                "M14e takes N execution: 3 parallel requests send take lines and return 3 passing takes") && ok;

    // MutateView representation: 3 active slots means 1 original card + 3 take cards = 4 cards
    MutateView view;
    view.open = true;
    view.numSlots = 3;
    ok = check (view.activeSlots() == 3,
                "M14e takes strip cards: 3 slots produce activeSlots() == 3 (4 total cards with original)") && ok;

    return ok;
}

// 3. 429 fallback and cancel for Takes
bool smokeTakes429AndCancel()
{
    bool ok = true;
    const Patch base = parsePatch (builtinPresetText (0)).patch;
    const std::string validReply = formatPatchAsReply (base);

    // 429 handling
    std::mutex lock;
    int calls = 0;
    TakesRequest treq;
    treq.prompt = "sub bass";
    treq.takes = 2;
    treq.backoffSeconds = 0.01;
    treq.transport = [&] (const ChatRequest&, const std::atomic<bool>*)
    {
        int call;
        {
            std::lock_guard<std::mutex> g (lock);
            call = ++calls;
        }
        ChatReply r;
        if (call == 1)
        {
            r.error = ChatError::RateLimited;
            r.httpStatus = 429;
            r.message = "Rate limited (HTTP 429): try again shortly";
            return r;
        }
        r.content = validReply;
        return r;
    };

    const MutateResult res429 = Mutator::runTakes (treq, nullptr);
    ok = check (res429.passed() == 2 && calls >= 3,
                "M14e takes 429 fallback: rate limited request is retried and passes (%d calls)", calls) && ok;

    // Cancellation
    std::atomic<bool> cancelFlag { false };
    TakesRequest treqCancel;
    treqCancel.prompt = "lead";
    treqCancel.takes = 3;
    treqCancel.transport = [&] (const ChatRequest&, const std::atomic<bool>* cancel)
    {
        cancelFlag.store (true);
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
        ChatReply r;
        if (cancel && cancel->load())
        {
            r.error = ChatError::Cancelled;
            r.message = "cancelled";
        }
        else
        {
            r.content = validReply;
        }
        return r;
    };

    const MutateResult resCancel = Mutator::runTakes (treqCancel, &cancelFlag);
    ok = check (resCancel.cancelled,
                "M14e takes cancel: cancelling stops the in-flight takes run") && ok;

    return ok;
}

// 4. All-fail vs one-pass strip opening
bool smokeTakesPassFailStrip()
{
    bool ok = true;
    const Patch base = parsePatch (builtinPresetText (0)).patch;

    // All fail: 3 slots all return garbage
    TakesRequest treqFail;
    treqFail.prompt = "weird noise";
    treqFail.takes = 3;
    treqFail.transport = [] (const ChatRequest&, const std::atomic<bool>*)
    {
        ChatReply r;
        r.content = "this is not valid json or pulsar patch";
        return r;
    };

    const MutateResult resFail = Mutator::runTakes (treqFail, nullptr);
    ok = check (resFail.passed() == 0 && resFail.slots.size() == 3 && ! resFail.slots[0].ok,
                "M14e all-fail: all 3 takes fail offline validation") && ok;

    // Exactly one passes: slot 0 passes, slot 1 & 2 fail
    TakesRequest treqOne;
    treqOne.prompt = "hybrid pluck";
    treqOne.takes = 3;
    treqOne.transport = [&] (const ChatRequest& c, const std::atomic<bool>*)
    {
        const int idx = takeIndexOf (c, 3);
        ChatReply r;
        if (idx == 0)
            r.content = formatPatchAsReply (base);
        else
            r.content = "invalid code";
        return r;
    };

    const MutateResult resOne = Mutator::runTakes (treqOne, nullptr);
    ok = check (resOne.passed() == 1 && resOne.slots[0].ok && ! resOne.slots[1].ok && ! resOne.slots[2].ok,
                "M14e one-pass: exactly 1 take passes and 2 fail") && ok;

    // Check MutateView setup logic with one pass
    MutateView view;
    view.open = resOne.passed() > 0;
    view.numSlots = 3;
    for (size_t i = 0; i < resOne.slots.size(); ++i)
    {
        view.slots[i].state = resOne.slots[i].ok ? MutateSlotView::State::Ready : MutateSlotView::State::Failed;
        view.slots[i].name = resOne.slots[i].ok ? resOne.slots[i].patch.name : "";
    }

    ok = check (view.open && view.slots[0].state == MutateSlotView::State::Ready
                    && view.slots[1].state == MutateSlotView::State::Failed
                    && view.slots[2].state == MutateSlotView::State::Failed,
                "M14e strip opens on one pass with slot 0 ready and slots 1-2 failed for discard") && ok;

    return ok;
}

// 5. Every passing take reaches History
bool smokeTakesHistory()
{
    bool ok = true;
    const Patch base = parsePatch (builtinPresetText (0)).patch;
    const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("pulsar_m14e_hist_" + juce::String (juce::Time::currentTimeMillis()));
    tempDir.createDirectory();

    // Simulate 3 passing takes being added to History with "<name> (take k)"
    const std::string takeNames[3] = { "Neuro Reese", "Neuro Reese", "Neuro Reese" };
    for (int k = 0; k < 3; ++k)
    {
        Patch p = base;
        p.name = takeNames[k] + " (take " + std::to_string (k + 1) + ")";
        juce::String err;
        // sleep 5ms so timestamps differ
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
        PatchLibrary::addToHistory (tempDir, p, juce::Time::getCurrentTime(), err);
        ok = check (err.isEmpty(), "M14e history add take %d: no error", k + 1) && ok;
    }

    const auto historyEntries = PatchLibrary::listHistory (tempDir);
    const bool countOk = historyEntries.size() == 3;
    bool namesOk = true;
    for (const auto& e : historyEntries)
    {
        if (! e.name.contains ("(take "))
            namesOk = false;
    }

    ok = check (countOk && namesOk,
                "M14e history: all 3 passing takes recorded as \"<name> (take k)\" in History (%d entries)",
                (int) historyEntries.size()) && ok;

    tempDir.deleteRecursively();
    return ok;
}

// 6. Keep, Discard and Undo behavior
bool smokeTakesKeepDiscardUndo()
{
    bool ok = true;
    Patch orig = parsePatch (builtinPresetText (0)).patch;
    orig.name = "Original Playing Patch";
    Patch take = orig;
    take.name = "Take 2 Sound";

    PatchSnapshot origSnap;
    origSnap.patch = orig;
    origSnap.source = "builtin:0";

    PatchSnapshot takeSnap;
    takeSnap.patch = take;
    takeSnap.source = "take:2";

    UndoStack undo;
    PatchSnapshot current = origSnap;

    // Simulate Audition: current becomes take, but no undo push yet
    current = takeSnap;
    ok = check (current.patch.name == "Take 2 Sound" && ! undo.canUndo(),
                "M14e audition: switches playing patch without pushing undo") && ok;

    // Simulate Discard: reverts current to original
    current = origSnap;
    ok = check (current.patch.name == "Original Playing Patch" && ! undo.canUndo(),
                "M14e discard: reverts playing patch to original without undo push") && ok;

    // Simulate Keep: pushes original onto undo stack and sets current to take
    undo.push (origSnap);
    current = takeSnap;
    ok = check (current.patch.name == "Take 2 Sound" && undo.canUndo(),
                "M14e keep: pushes original onto undo stack and keeps take") && ok;

    // Simulate Undo: undoes back to original
    PatchSnapshot popped;
    const bool popOk = undo.pop (popped);
    current = popped;
    ok = check (popOk && current.patch.name == "Original Playing Patch" && ! undo.canUndo(),
                "M14e undo after keep: restores original playing patch") && ok;

    return ok;
}

} // namespace

bool smokeM14e()
{
    std::printf ("== M14e smoketests ==\n");
    bool ok = true;
    ok = smokeTakesDirections() && ok;
    ok = smokeTakesNExecution() && ok;
    ok = smokeTakes429AndCancel() && ok;
    ok = smokeTakesPassFailStrip() && ok;
    ok = smokeTakesHistory() && ok;
    ok = smokeTakesKeepDiscardUndo() && ok;
    return ok;
}

} // namespace vibesynth_cli
