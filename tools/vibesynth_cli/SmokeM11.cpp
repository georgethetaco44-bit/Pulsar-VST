// M11 smoketests. See SmokeM11.h.

#include "SmokeM11.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "CliCheck.h"
#include "ai/Generator.h"
#include "ai/Mutator.h"
#include "ai/NodeId.h"
#include "ai/PromptBuilder.h"
#include "ai/ResponseParser.h"
#include "ai/UndoStack.h"
#include "patch/Patch.h"
#include "patch/PatchValidator.h"

namespace vibesynth_cli {

namespace {

using namespace vibesynth;
using Clock = std::chrono::steady_clock;

Patch builtin (const char* stem)
{
    for (int i = 0; i < numBuiltinPresets(); ++i)
        if (juce::String (builtinPresetFile (i)) == juce::String (stem) + ".pulsar")
            return parsePatch (builtinPresetText (i)).patch;
    return {};
}

std::string replaced (const std::string& s, const std::string& from, const std::string& to)
{
    return juce::String (s).replace (from, to).toStdString();
}

// Counts how many callers are inside at once (the most ever seen).
struct Concurrency
{
    std::atomic<int> now { 0 };
    std::atomic<int> most { 0 };

    void enter()
    {
        const int n = ++now;
        int m = most.load();
        while (n > m && ! most.compare_exchange_weak (m, n))
        {
        }
    }
    void leave() { --now; }
};

std::string summary (const GenerationResult& r)
{
    std::string s;
    for (const auto& a : r.attempts)
        s += (s.empty() ? "" : " | ") + a.stage + (a.reason.empty() ? "" : ": " + a.reason.substr (0, 80));
    return s.empty() ? (r.reason.empty() ? "no attempts" : r.reason) : s;
}

bool smokeNodeIds()
{
    bool ok = true;
    struct Case { const char* id; bool valid; const char* canonical; };
    const Case cases[] = {
        { "flt_lp:0", true, "flt_lp:0" },
        { "master/bus_sat#1", true, "master/bus_sat#1" },
        { "flt_lp 2", true, "flt_lp:2" },             // the node view's label
        { "fx_sat #1", true, "fx_sat#1" },             // a shaper's label
        { "  Master/FLT_LP:1 ", true, "master/flt_lp:1" },
        { "voice/osc_supersaw:1", true, "osc_supersaw:1" },
        { "fx_fold", true, "fx_fold#0" },              // no slot = 0 (as in graph.edges)
        { "util_xover_hi:0", true, "util_xover:0" },   // an alias
        { "osc_wavetable:0", false, "" },
        { "flt_lp:9", false, "" },
        { "flt_lp:x", false, "" },
        { "", false, "" },
    };
    int good = 0;
    std::string bad;
    for (const auto& c : cases)
    {
        NodeRef n;
        std::string error;
        const bool parsed = parseNodeId (c.id, n, error);
        const bool right = parsed == c.valid && (! parsed || nodeIdString (n) == c.canonical)
                        && (parsed || ! error.empty());
        good += right ? 1 : 0;
        if (! right)
            bad += std::string (" [") + c.id + " -> " + (parsed ? nodeIdString (n) : "refused: " + error) + "]";
    }
    ok = check (good == (int) std::size (cases), "node ids: %d/%d forms parse to the right node or are refused%s",
                good, (int) std::size (cases), bad.c_str()) && ok;

    NodeRef shaper;
    std::string error;
    parseNodeId ("master/bus_sat#1", shaper, error);
    const std::string d = describeNodeForPrompt (shaper);
    NodeRef filter;
    parseNodeId ("flt_lp:0", filter, error);
    const std::string f = describeNodeForPrompt (filter);
    ok = check (d.find ("2nd bus_sat(...) call in the master section") != std::string::npos
                    && f.find ("flt_lp(0, ...) call in the voice section") != std::string::npos,
                "node ids: prompt lines \"%s\" / \"%s\"", f.c_str(), d.c_str()) && ok;

    const Patch reese = builtin ("reese");
    const TouchedNodes t = probeTouchedNodes (reese, 48000.0);
    NodeRef sat0 = shaper;
    sat0.slot = 0;
    TouchedNodes fewer = t;
    fewer.voice[(int) filter.module] = 0u;
    ok = check (nodeIsTouched (t, filter) && nodeIsTouched (t, shaper) && nodeIsTouched (t, sat0)
                    && lostNodes (t, t).empty() && lostNodes (t, fewer) == "flt_lp:0",
                "node ids: Reese touches flt_lp:0 and master bus_sat #0/#1; a dropped node is reported (\"%s\")",
                lostNodes (t, fewer).c_str()) && ok;
    return ok;
}

bool smokeReplyFormat()
{
    int same = 0;
    std::string bad;
    for (int i = 0; i < numBuiltinPresets(); ++i)
    {
        const Patch p = parsePatch (builtinPresetText (i)).patch;
        const auto r = parseResponse (formatPatchAsReply (p));
        Patch want = p;
        while ((int) want.macros.size() < Patch::kMaxMacros)
            want.macros.push_back (MacroSpec{});
        const bool eq = r.ok() && r.patch.name == want.name && r.patch.voiceMode == want.voiceMode
                     && r.patch.macros == want.macros && r.patch.graphJson == want.graphJson
                     && r.patch.init == want.init && r.patch.voice == want.voice && r.patch.master == want.master;
        same += eq ? 1 : 0;
        if (! eq)
            bad += " " + std::string (builtinPresetFile (i)) + (r.ok() ? "" : " (" + r.error + ")");
    }
    return check (same == numBuiltinPresets(),
                  "reply format: %d/%d built-ins read back from formatPatchAsReply unchanged (macros padded to 8)%s",
                  same, numBuiltinPresets(), bad.c_str());
}

bool smokeEditResources()
{
    const auto& res = PromptResources::embedded();
    bool ok = true;
    const int changeTokens = estimateTokens (res.changeInstructions);
    const int mutateTokens = estimateTokens (res.mutateInstructions);
    ok = check (! res.changeInstructions.empty() && changeTokens <= 250
                    && res.changeInstructions.find ("{node}") != std::string::npos
                    && res.changeInstructions.find ("{instruction}") != std::string::npos
                    && ! res.mutateInstructions.empty() && mutateTokens <= 250
                    && res.mutateInstructions.find ("{direction}") != std::string::npos,
                "edit prompts: change block ~%d tokens, mutate block ~%d tokens (each <= 250, placeholders present)",
                changeTokens, mutateTokens) && ok;
    std::string keys;
    bool texts = true;
    for (const auto& d : res.mutateDirections)
    {
        keys += (keys.empty() ? "" : ",") + d.key;
        texts = texts && ! d.label.empty() && d.text.size() > 40;
    }
    ok = check (keys == "darker,movement,brighter,weirder" && texts,
                "edit prompts: 4 mutate directions in order (%s), each labelled with a text", keys.c_str()) && ok;
    // The edit files are not recipes or examples.
    bool leaked = false;
    for (const auto& r : res.recipes)
        leaked = leaked || r.file.rfind ("edit_", 0) == 0;
    ok = check (! leaked, "edit prompts: not picked up as recipes (%d recipes)", (int) res.recipes.size()) && ok;
    return ok;
}

bool smokeChange()
{
    bool ok = true;
    const Patch reese = builtin ("reese");
    const std::string oldCut = "cut = min(0.97, 0.12 + m2 * 0.55 + env * 0.30);";
    const std::string newCut = "wob = 0.5 - 0.5 * cos(beat * 2 * $pi);\n"
                               "cut = min(0.97, 0.12 + m2 * 0.55 * (1 - m3 * wob) + env * 0.30);";
    Patch changed = reese;
    changed.voice = replaced (reese.voice, oldCut, newCut);
    const bool edited = changed.voice != reese.voice;

    std::vector<ChatRequest> seen;
    std::string reply = formatPatchAsReply (changed);
    GenerationRequest gr;
    gr.kind = GenerationKind::Change;
    gr.base = reese;
    gr.node = "flt_lp:0";
    gr.instruction = "make this filter wobble at 1/4 notes";
    gr.measureVoice = [] (const Patch& p) { return p.voiceBudgetNs() * 0.1; };
    gr.transport = [&] (const ChatRequest& c)
    {
        seen.push_back (c);
        ChatReply r;
        r.content = reply;
        r.promptTokens = 3000;
        r.completionTokens = 900;
        return r;
    };
    GenerationResult res = Generator::run (gr, nullptr);
    const BuiltPrompt gen = PromptBuilder().build ("anything");
    const std::string user = seen.empty() ? "" : seen[0].messages.back().content;
    ok = check (edited && res.ok && seen.size() == 1 && seen[0].messages.size() == 2
                    && seen[0].messages[0].content == gen.messages[0].content
                    && seen[0].temperature == gr.config.generateTemperature
                    && user.find ("The current patch:\n\n" + formatPatchAsReply (reese)) != std::string::npos
                    && user.find ("Node: flt_lp:0: the flt_lp(0, ...) call in the voice section") != std::string::npos
                    && user.find ("Instruction: make this filter wobble at 1/4 notes") != std::string::npos
                    && user.find ("Keep every existing slot number") != std::string::npos
                    && user.find ("{node}") == std::string::npos,
                "change: one request with the generate system prompt, the patch in reply format, the node and "
                "the instruction (%d messages, %d chars; %s)",
                seen.empty() ? 0 : (int) seen[0].messages.size(), (int) user.size(), summary (res).c_str()) && ok;
    ok = check (res.ok && res.patch.voice == changed.voice && res.lostNodes.empty() && res.labelChanges.empty()
                    && res.patch.prompt == reese.prompt + "\nchange flt_lp:0: make this filter wobble at 1/4 notes"
                    && res.patch.trim > 0.0 && res.patch.trim <= 1.0,
                "change: the edit passes, keeps every node and label (dropped: \"%s\"), prompt records the change",
                res.lostNodes.c_str()) && ok;

    // A reply that renumbers the filter's slot: still valid, reported.
    Patch moved = changed;
    moved.voice = replaced (changed.voice, "flt_lp(0,", "flt_lp(1,");
    moved.macros[1].label = "Wobble";
    reply = formatPatchAsReply (moved);
    seen.clear();
    res = Generator::run (gr, nullptr);
    ok = check (res.ok && res.lostNodes == "flt_lp:0" && res.labelChanges.size() == 1
                    && res.labelChanges[0] == "m2 Cutoff -> Wobble",
                "change: a renumbered slot and a relabelled macro are reported (dropped \"%s\", %s)",
                res.lostNodes.c_str(), res.labelChanges.empty() ? "no label change" : res.labelChanges[0].c_str()) && ok;

    // Nodes the patch doesn't have, or ids that aren't nodes: refused
    // before any request.
    reply = formatPatchAsReply (changed);
    for (const char* bad : { "flt_lp:2", "osc_wavetable:0" })
    {
        seen.clear();
        gr.node = bad;
        res = Generator::run (gr, nullptr);
        ok = check (! res.ok && seen.empty() && res.attempts.empty()
                        && (res.reason.find ("isn't in this patch") != std::string::npos
                            || res.reason.find ("unknown node") != std::string::npos),
                    "change: node %s refused without a request (\"%s\")", bad, res.reason.c_str()) && ok;
    }

    // The master-shaper id form.
    seen.clear();
    gr.node = "master/bus_sat#1";
    gr.instruction = "add grit before this";
    res = Generator::run (gr, nullptr);
    const std::string u2 = seen.empty() ? "" : seen[0].messages.back().content;
    ok = check (res.ok && u2.find ("Node: master/bus_sat#1: the 2nd bus_sat(...) call in the master section")
                              != std::string::npos,
                "change: master/bus_sat#1 accepted and described (%s)", summary (res).c_str()) && ok;

    // Self-heal works the same: a lint failure is retried with the reason.
    seen.clear();
    gr.node = "flt_lp:0";
    std::vector<std::string> replies = { replaced (formatPatchAsReply (changed), "flt_lp(0,", "flt_wobble(0,"),
                                         formatPatchAsReply (changed) };
    gr.transport = [&] (const ChatRequest& c)
    {
        seen.push_back (c);
        ChatReply r;
        r.content = replies.at (std::min (seen.size(), replies.size()) - 1);
        return r;
    };
    res = Generator::run (gr, nullptr);
    ok = check (res.ok && res.attempts.size() == 2 && res.attempts[0].stage == "lint" && seen.size() == 2
                    && seen[1].temperature == gr.config.retryTemperature
                    && seen[1].messages.back().content.find ("flt_wobble") != std::string::npos,
                "change: self-heal retries a lint failure with the reason (%s)", summary (res).c_str()) && ok;
    return ok;
}

// Which direction a mutate request is for (its text is in the last user message).
int directionOf (const ChatRequest& c)
{
    const auto& dirs = PromptResources::embedded().mutateDirections;
    for (const auto& m : c.messages)
        if (m.role == "user")
            for (size_t i = 0; i < dirs.size(); ++i)
                if (m.content.find (dirs[i].text) != std::string::npos)
                    return (int) i;
    return -1;
}

bool smokeMutate()
{
    bool ok = true;
    const Patch reese = builtin ("reese");
    auto variant = [&] (const char* name)
    {
        Patch p = reese;
        p.name = name;
        return formatPatchAsReply (p);
    };
    const std::string replies[4] = { variant ("Dark Reese"), "no blocks here", variant ("Bright Reese"),
                                     variant ("Odd Reese") };

    // Slot 0 ok, slot 1 garbage (fails after 2 retries), slot 2 a 429 then
    // ok, slot 3 ok. Every request sleeps a little so they overlap.
    std::mutex lock;
    std::vector<double> temps;
    std::map<int, int> calls;
    Concurrency http, testing;
    MutateRequest mr;
    mr.base = reese;
    mr.backoffSeconds = 0.01;
    mr.measureVoice = [&] (const Patch& p)
    {
        testing.enter();
        std::this_thread::sleep_for (std::chrono::milliseconds (30));
        testing.leave();
        return p.voiceBudgetNs() * 0.1;
    };
    mr.transport = [&] (const ChatRequest& c, const std::atomic<bool>*)
    {
        http.enter();
        std::this_thread::sleep_for (std::chrono::milliseconds (60));
        http.leave();
        const int d = directionOf (c);
        int call = 0;
        {
            std::lock_guard<std::mutex> g (lock);
            call = ++calls[d];
            temps.push_back (c.temperature);
        }
        ChatReply r;
        if (d == 2 && call == 1)
        {
            r.error = ChatError::RateLimited;
            r.httpStatus = 429;
            r.message = "Rate limited (HTTP 429): try again shortly";
            return r;
        }
        r.content = d >= 0 ? replies[d] : "";
        r.promptTokens = 3000;
        r.completionTokens = 800;
        return r;
    };
    std::vector<int> doneOrder;
    const MutateResult res = Mutator::run (mr, nullptr, {}, [&] (int slot, const GenerationResult&)
    {
        std::lock_guard<std::mutex> g (lock);
        doneOrder.push_back (slot);
    });
    const bool shape = res.slots.size() == 4 && res.directions.size() == 4 && doneOrder.size() == 4;
    ok = check (shape && res.slots[0].ok && ! res.slots[1].ok && res.slots[2].ok && res.slots[3].ok
                    && res.slots[0].patch.name == "Dark Reese" && res.slots[3].patch.name == "Odd Reese"
                    && res.slots[1].attempts.size() == 3 && res.slots[1].reason.find ("no ```json") == 0,
                "mutate: 4 canned slots -> 3 pass, the garbage slot fails with its mapped reason (\"%s\")",
                shape ? res.slots[1].reason.substr (0, 60).c_str() : "?") && ok;
    ok = check (shape && res.rateLimited[2] == 1 && calls[2] == 2 && res.slots[2].attempts.size() == 1
                    && ! res.wentSequential,
                "mutate: one 429 is backed off and retried without using a self-heal attempt (%d calls, %d 429s)",
                calls[2], shape ? res.rateLimited[2] : -1) && ok;
    const bool firstTemps = std::count (temps.begin(), temps.end(), mr.config.mutateTemperature) >= 4;
    ok = check (http.most.load() >= 3 && testing.most.load() == 1 && firstTemps,
                "mutate: requests run in parallel (%d at once), offline tests one at a time (%d at once), "
                "first attempts at %.2f",
                http.most.load(), testing.most.load(), mr.config.mutateTemperature) && ok;
    ok = check (shape && res.slots[0].patch.prompt == reese.prompt + "\nmutate: Darker" && res.passed() == 3
                    && res.promptTokens() > 0,
                "mutate: results carry the direction in their prompt; %d tokens counted", res.promptTokens()) && ok;

    // Persistent rate limiting: every slot's first two requests get a 429.
    // The second 429 switches to one request at a time; the third request
    // of every slot (after its second 429) must then run alone.
    calls.clear();
    Concurrency late;
    mr.measureVoice = [] (const Patch& p) { return p.voiceBudgetNs() * 0.1; };
    mr.transport = [&] (const ChatRequest& c, const std::atomic<bool>*)
    {
        const int d = directionOf (c);
        int call = 0;
        {
            std::lock_guard<std::mutex> g (lock);
            call = ++calls[d];
        }
        if (call >= 3)
            late.enter();
        std::this_thread::sleep_for (std::chrono::milliseconds (40));
        if (call >= 3)
            late.leave();
        ChatReply r;
        if (call <= 2)
        {
            r.error = ChatError::RateLimited;
            r.httpStatus = 429;
            r.message = "Rate limited (HTTP 429): try again shortly";
            return r;
        }
        r.content = variant ("Again");
        return r;
    };
    const MutateResult seq = Mutator::run (mr, nullptr);
    ok = check (seq.passed() == 4 && seq.wentSequential && late.most.load() == 1,
                "mutate: repeated 429s fall back to sequential requests (%d/4 pass, %d at once after the fallback)",
                seq.passed(), late.most.load()) && ok;

    // A 429 that never ends: the slot fails with the mapped message.
    mr.transport = [&] (const ChatRequest&, const std::atomic<bool>*)
    {
        ChatReply r;
        r.error = ChatError::RateLimited;
        r.httpStatus = 429;
        r.message = "Rate limited (HTTP 429): try again shortly";
        return r;
    };
    const MutateResult stuck = Mutator::run (mr, nullptr);
    ok = check (stuck.passed() == 0 && stuck.slots.size() == 4 && stuck.rateLimited[0] == Mutator::kRateLimitRetries
                    && stuck.slots[0].reason.find ("Rate limited") == 0,
                "mutate: a 429 that persists fails each slot after %d back-offs (\"%s\")", Mutator::kRateLimitRetries,
                stuck.slots.empty() ? "?" : stuck.slots[0].reason.c_str()) && ok;

    // isRateLimit also reads a 5xx/4xx message that says so.
    ChatReply text;
    text.error = ChatError::Server;
    text.message = "Server error (HTTP 503): Rate limit exceeded";
    ChatReply plain;
    plain.error = ChatError::Server;
    plain.message = "Server error (HTTP 500)";
    ok = check (Mutator::isRateLimit (text) && ! Mutator::isRateLimit (plain),
                "mutate: a server message saying \"rate limit\" counts as rate limiting") && ok;
    return ok;
}

bool smokeMutateCancel()
{
    const Patch reese = builtin ("reese");
    MutateRequest mr;
    mr.base = reese;
    mr.transport = [] (const ChatRequest&, const std::atomic<bool>* cancel)
    {
        // A request that hangs until cancelled (as MetaClient polls it).
        const auto start = Clock::now();
        while (cancel != nullptr && ! cancel->load() && Clock::now() - start < std::chrono::seconds (5))
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        ChatReply r;
        r.error = ChatError::Cancelled;
        r.message = "cancelled";
        return r;
    };
    std::atomic<bool> cancel { false };
    MutateResult res;
    std::atomic<int> waiting { 0 };
    const auto t0 = Clock::now();
    std::thread t ([&]
    {
        res = Mutator::run (mr, &cancel, [&] (int, const GenerationProgress& p)
        {
            if (p.stage == GenerationStage::Waiting)
                ++waiting;
        });
    });
    // Cancel once all 4 are waiting (or after 2 s).
    while (waiting.load() < 4 && Clock::now() - t0 < std::chrono::seconds (2))
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    const auto tc = Clock::now();
    cancel = true;
    t.join();
    const double ms = std::chrono::duration<double, std::milli> (Clock::now() - tc).count();
    int cancelled = 0;
    for (const auto& s : res.slots)
        cancelled += s.cancelled ? 1 : 0;
    return check (res.cancelled && cancelled == 4 && ms < 1000.0,
                  "mutate: Cancel stops all 4 slots (%d cancelled, %.0f ms after the flag)", cancelled, ms);
}

bool smokeUndoStack()
{
    bool ok = true;
    UndoStack stack;
    auto snap = [] (int i)
    {
        PatchSnapshot s;
        s.patch = builtin (i % 2 == 0 ? "reese" : "pluck");
        s.patch.name += " " + std::to_string (i);
        s.patch.trim = 0.5 + i * 0.01;
        for (int m = 0; m < PatchSnapshot::kNumMacros; ++m)
            s.macros[(size_t) m] = 0.123456789f * (float) (m + 1) / (float) (i + 1);
        s.source = "builtin:" + std::to_string (i);
        s.program = i;
        s.nodes.voice[2] = (uint32_t) i;
        s.nodesKnown = true;
        if (i == 3)  // M10's tweaks ride in the patch
            s.patch.tweaks.push_back ({ "flt_lp:0", false, { { "cutoff", ArgTweak::kOffset, 0.12 } } });
        return s;
    };
    for (int i = 0; i < 22; ++i)
        stack.push (snap (i));
    ok = check (stack.size() == UndoStack::kDepth, "undo: the stack keeps the newest %d of 22 entries",
                (int) stack.size()) && ok;
    int exact = 0;
    PatchSnapshot out;
    for (int i = 21; i >= 2 && stack.pop (out); --i)
        exact += out.sameAs (snap (i)) ? 1 : 0;
    ok = check (exact == 20 && ! stack.canUndo() && ! stack.pop (out),
                "undo: pops newest first and restores patch (with its tweaks), trim, macro values, source and "
                "nodes bit-exactly (%d/20), then is empty", exact) && ok;
    PatchSnapshot a = snap (1), b = snap (1);
    b.macros[4] = std::nextafter (b.macros[4], 1.0f);
    ok = check (! a.sameAs (b), "undo: a macro one float step away counts as different") && ok;
    return ok;
}

} // namespace

bool smokeM11()
{
    bool ok = true;
    ok = smokeNodeIds() && ok;
    ok = smokeReplyFormat() && ok;
    ok = smokeEditResources() && ok;
    ok = smokeChange() && ok;
    ok = smokeMutate() && ok;
    ok = smokeMutateCancel() && ok;
    ok = smokeUndoStack() && ok;
    return ok;
}

} // namespace vibesynth_cli
