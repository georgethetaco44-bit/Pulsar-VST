// M10 + M11 merge smoketests. See SmokeMerge.h.

#include "SmokeMerge.h"

#include <juce_core/juce_core.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "CliCheck.h"
#include "ai/Generator.h"
#include "ai/Mutator.h"
#include "ai/NodeId.h"
#include "ai/ResponseParser.h"
#include "ai/UndoStack.h"
#include "engine/NodeOverrides.h"
#include "patch/Patch.h"
#include "patch/PatchBake.h"
#include "patch/PatchTweaks.h"
#include "patch/PatchValidator.h"

namespace vibesynth_cli {

namespace {

using namespace vibesynth;

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

bool sameBits (double a, double b)
{
    return std::memcmp (&a, &b, sizeof (double)) == 0;
}

// Every tweak and map number, bit for bit (Patch::operator== allows the
// .pulsar rounding; Undo must not).
bool tweaksBitExact (const Patch& a, const Patch& b)
{
    if (a.tweaks.size() != b.tweaks.size() || a.macroMaps.size() != b.macroMaps.size())
        return false;
    for (size_t i = 0; i < a.tweaks.size(); ++i)
    {
        const auto& x = a.tweaks[i];
        const auto& y = b.tweaks[i];
        if (x.node != y.node || x.bypass != y.bypass || x.args.size() != y.args.size())
            return false;
        for (size_t k = 0; k < x.args.size(); ++k)
            if (x.args[k].arg != y.args[k].arg || x.args[k].mode != y.args[k].mode
                || ! sameBits (x.args[k].value, y.args[k].value))
                return false;
    }
    for (size_t i = 0; i < a.macroMaps.size(); ++i)
    {
        const auto& x = a.macroMaps[i];
        const auto& y = b.macroMaps[i];
        if (x.macro != y.macro || x.node != y.node || x.arg != y.arg || ! sameBits (x.min, y.min)
            || ! sameBits (x.max, y.max))
            return false;
    }
    return true;
}

// What the audio thread reads, compared field by field.
bool sameTables (const OverrideTable& a, const OverrideTable& b)
{
    for (int r = 0; r < kNumRackKinds; ++r)
        for (int m = 0; m < kNumModules; ++m)
        {
            if (a.active[r][m].load() != b.active[r][m].load())
                return false;
            for (int s = 0; s < kMaxSlotsPerModule; ++s)
            {
                const auto& x = a.nodes[r][m][s];
                const auto& y = b.nodes[r][m][s];
                if (x.bypass.load() != y.bypass.load())
                    return false;
                // Values behind a cleared mode are never read.
                for (int k = 0; k < kMaxLexiconArgs; ++k)
                    if (x.args[k].mode.load() != y.args[k].mode.load()
                        || (x.args[k].mode.load() != 0
                            && (x.args[k].macro.load() != y.args[k].macro.load()
                                || ! sameBits (x.args[k].applied.load(), y.args[k].applied.load())
                                || ! sameBits (x.args[k].mapLo.load(), y.args[k].mapLo.load())
                                || ! sameBits (x.args[k].mapHi.load(), y.args[k].mapHi.load()))))
                        return false;
            }
        }
    return true;
}

// Reese as a user might shape it: the filter's cutoff offset, its
// resonance on the unused macro 5, the fold bypassed and its amount on
// macro 6.
Patch shapedReese()
{
    Patch p = builtin ("reese");
    setPatchArgTweak (p, "flt_lp:0", "cutoff", ArgTweak::kOffset, 0.123456789);
    setPatchMacroMap (p, "flt_lp:0", "res", 5, 0.1, 0.65);
    setPatchBypass (p, "fx_fold#0", true);
    setPatchMacroMap (p, "fx_fold#0", "amount", 6, 0.0, 0.4);
    setPatchArgTweak (p, "master/bus_sat#1", "drive", ArgTweak::kReplace, 0.2);
    return p;
}

// Node ids: every node of every built-in, through the one implementation.
bool smokeBuiltinNodeIds()
{
    int nodes = 0, good = 0, slotless = 0;
    std::string bad;
    for (int i = 0; i < numBuiltinPresets(); ++i)
    {
        const auto pr = parsePatch (builtinPresetText (i));
        if (! pr.ok())
        {
            bad += std::string (" [") + builtinPresetFile (i) + " doesn't parse]";
            continue;
        }
        const TouchedNodes t = probeTouchedNodes (pr.patch, 48000.0);
        for (int rk = 0; rk < kNumRackKinds; ++rk)
            for (int m = 0; m < kNumModules; ++m)
                for (int s = 0; s < kMaxSlotsPerModule; ++s)
                {
                    const NodeRef n { (RackKind) rk, (ModuleId) m, s };
                    if (! nodeIsTouched (t, n))
                        continue;
                    ++nodes;
                    const std::string id = nodeIdString (n);
                    NodeRef back;
                    std::string error;
                    // The id, and the node view's label spelling of it
                    // ("flt_lp 0", "fx_sat #1"; the view shows the region apart).
                    const std::string label = replaced (replaced (id, ":", " "), "#", " #");
                    NodeRef fromLabel;
                    const bool ok = parseNodeId (id, back, error) && back == n && nodeIdString (back) == id
                                 && nodeIsTouched (t, back) && parseNodeId (label, fromLabel) && fromLabel == n;
                    slotless += id.find_first_of (":#") == std::string::npos ? 1 : 0;
                    good += ok ? 1 : 0;
                    if (! ok)
                        bad += " [" + std::string (builtinPresetFile (i)) + " " + id + "]";
                }
    }
    return check (nodes > 0 && good == nodes && slotless > 0,
                  "merge node ids: %d/%d nodes of the %d built-ins round-trip through parseNodeId/nodeIdString "
                  "(and their node-view label), %d slotless bus ids like master/bus_mono_low%s",
                  good, nodes, numBuiltinPresets(), slotless, bad.c_str());
}

// Undo: the snapshot's patch carries the tweaks and macro maps; the
// processor reloads it as .pulsar text (applySnapshot -> processJob) and
// applies the table from it (handleAsyncUpdate -> applyTweaksFrom).
bool smokeUndoTweaks()
{
    bool ok = true;
    const Patch shaped = shapedReese();
    PatchSnapshot before;
    before.patch = shaped;
    const float macros[8] = { 0.1234567f, 0.9876543f, 0.000001f, 1.0f, 0.3333333f, 0.7071068f, 0.5f, 0.0f };
    std::memcpy (before.macros.data(), macros, sizeof (macros));
    before.source = "builtin:0";
    before.program = 0;
    before.nodes = probeTouchedNodes (shaped, 48000.0);
    before.nodesKnown = true;

    UndoStack stack;
    stack.push (before);
    // A preset load (no tweaks) swaps in; then Undo.
    PatchSnapshot popped;
    const bool got = stack.pop (popped);
    const auto reparsed = parsePatch (serializePatch (popped.patch));
    const Patch& restored = reparsed.patch;

    OverrideTable want, have;
    applyPatchTweaks (shaped, want);
    applyPatchTweaks (builtin ("pluck"), have);  // what the preset load left
    const int applied = applyPatchTweaks (restored, have);
    ok = check (got && reparsed.ok() && restored == shaped && tweaksBitExact (restored, shaped)
                    && popped.sameAs (before) && restored.tweaks.size() == 3 && restored.macroMaps.size() == 2
                    && sameTables (want, have) && applied == 5,
                "merge undo: a shaped Reese (3 tweaked nodes, 2 macro maps) comes back from the stack and its "
                "reload bit-exactly, macros included, and rebuilds the same override table (%d applied)", applied)
       && ok;

    // Knob edits don't push: only swaps do (the stack holds what it held).
    UndoStack edits;
    edits.push (before);
    Patch live = shaped;
    setPatchArgTweak (live, "flt_lp:0", "cutoff", ArgTweak::kOffset, 0.3);
    ok = check (edits.size() == 1 && edits.top() != nullptr && tweaksBitExact (edits.top()->patch, shaped)
                    && ! tweaksBitExact (live, shaped),
                "merge undo: a knob edit after a swap changes the live patch, not the stacked one") && ok;
    return ok;
}

// Change: tweaks on surviving nodes stay, those on nodes the reply removed
// go and are named.
bool smokeChangeKeepsTweaks()
{
    bool ok = true;
    const Patch shaped = shapedReese();
    Patch changed = builtin ("reese");
    changed.voice = replaced (changed.voice, "y = fx_fold(y, m4 * 0.35);\n", "");
    changed.voice = replaced (changed.voice, "cut = min(0.97, 0.12 + m2 * 0.55 + env * 0.30);",
                              "wob = 0.5 - 0.5 * cos(beat * 2 * $pi);\n"
                              "cut = min(0.97, 0.12 + m2 * 0.55 * (1 - 0.5 * wob) + env * 0.30);");
    std::string sent;
    GenerationRequest gr;
    gr.kind = GenerationKind::Change;
    gr.base = shaped;
    gr.node = "flt_lp:0";
    gr.instruction = "make this filter wobble at 1/4 notes and drop the fold";
    gr.measureVoice = [] (const Patch& p) { return p.voiceBudgetNs() * 0.1; };
    gr.transport = [&] (const ChatRequest& c)
    {
        sent = c.messages.back().content;
        ChatReply r;
        r.content = formatPatchAsReply (changed);
        return r;
    };
    const GenerationResult res = Generator::run (gr, nullptr);

    Patch wantKept;
    setPatchArgTweak (wantKept, "flt_lp:0", "cutoff", ArgTweak::kOffset, 0.123456789);
    setPatchMacroMap (wantKept, "flt_lp:0", "res", 5, 0.1, 0.65);
    setPatchArgTweak (wantKept, "master/bus_sat#1", "drive", ArgTweak::kReplace, 0.2);
    std::string dropped;
    for (const auto& d : res.droppedTweaks)
        dropped += (dropped.empty() ? "" : "; ") + d;
    ok = check (res.ok && res.lostNodes == "fx_fold#0" && tweaksBitExact (res.patch, wantKept)
                    && res.droppedTweaks.size() == 2 && dropped == "fx_fold#0 (bypass); M6 -> fx_fold#0 amount"
                    && sent.find (formatPatchAsReply (builtin ("reese"))) != std::string::npos,
                "merge change: the tweaks on flt_lp:0 and master/bus_sat#1 and the M5 map stay, fx_fold#0's "
                "bypass and M6 map go and are named (\"%s\")", dropped.c_str())
       && ok;

    // A change that keeps every node keeps every tweak.
    Patch same = builtin ("reese");
    same.macros[1].label = "Tone";
    gr.transport = [&] (const ChatRequest&)
    {
        ChatReply r;
        r.content = formatPatchAsReply (same);
        return r;
    };
    const GenerationResult all = Generator::run (gr, nullptr);
    ok = check (all.ok && all.droppedTweaks.empty() && tweaksBitExact (all.patch, shaped),
                "merge change: when every node survives, every tweak and map does too") && ok;

    // Generate and Mutate never inherit tweaks.
    GenerationRequest gen = gr;
    gen.kind = GenerationKind::Generate;
    gen.prompt = "reese";
    const GenerationResult fresh = Generator::run (gen, nullptr);
    ok = check (fresh.ok && fresh.patch.tweaks.empty() && fresh.patch.macroMaps.empty(),
                "merge generate: a new patch starts with no tweaks") && ok;
    return ok;
}

// Mutate: tweaks are baked into the code sent; a refused bake sends the
// unbaked code and says so.
bool smokeMutateBakes()
{
    bool ok = true;
    const Patch shaped = shapedReese();
    const MutateBase mb = prepareMutateBase (shaped);
    const BakeResult direct = bakePatch (shaped);

    std::mutex lock;
    std::vector<std::string> sent;
    MutateRequest req;
    req.base = mb.patch;
    req.baseNodesKnown = false;
    req.measureVoice = [] (const Patch& p) { return p.voiceBudgetNs() * 0.1; };
    req.backoffSeconds = 0.01;
    req.transport = [&] (const ChatRequest& c, const std::atomic<bool>*)
    {
        {
            std::lock_guard<std::mutex> g (lock);
            sent.push_back (c.messages.back().content);
        }
        ChatReply r;
        r.content = formatPatchAsReply (mb.patch);  // a variation that changes nothing
        return r;
    };
    const MutateResult mr = Mutator::run (req, nullptr);
    const std::string bakedReply = formatPatchAsReply (direct.patch);
    int carried = 0, noTweaks = 0;
    for (const auto& s : sent)
        carried += s.find (bakedReply) != std::string::npos ? 1 : 0;
    for (const auto& r : mr.slots)
        noTweaks += r.ok && r.patch.tweaks.empty() && r.patch.macroMaps.empty() && r.patch.voice == direct.patch.voice
                        ? 1
                        : 0;
    const bool shapingInCode = direct.patch.voice.find ("m5") != std::string::npos
                            && direct.patch.voice.find ("flt_lp(0, x, cut, m3)") == std::string::npos
                            && direct.patch.voice.find ("fx_fold(") == std::string::npos;
    ok = check (mb.hadTweaks && mb.baked && mb.note.empty() && direct.ok && mb.patch.voice == direct.patch.voice
                    && mb.patch.tweaks.empty() && mb.patch.macroMaps.empty() && shapingInCode && sent.size() == 4
                    && carried == 4 && mr.passed() == 4 && noTweaks == 4,
                "merge mutate: with tweaks set, all 4 requests carry the baked code (cutoff offset, M5 res map, "
                "fold bypassed) and the variations start with no tweaks (%d/4 sent baked, %d/4 clean)",
                carried, noTweaks)
       && ok;

    // A tweaked shaper in a branch can't be baked: the unbaked code goes,
    // with a note.
    Patch branchy = builtin ("reese");
    branchy.voice = replaced (branchy.voice, "y = fx_sat(y + sub * 0.5, m4);",
                              "y = gate > 0 ? fx_sat(y + sub * 0.5, m4) : fx_sat(y, m4);");
    setPatchArgTweak (branchy, "fx_sat#0", "drive", ArgTweak::kReplace, 0.8);
    const MutateBase refused = prepareMutateBase (branchy);
    Patch unbaked = branchy;
    unbaked.tweaks.clear();
    sent.clear();
    req.base = refused.patch;
    req.transport = [&] (const ChatRequest& c, const std::atomic<bool>*)
    {
        {
            std::lock_guard<std::mutex> g (lock);
            sent.push_back (c.messages.back().content);
        }
        ChatReply r;
        r.content = formatPatchAsReply (unbaked);
        return r;
    };
    const MutateResult mr2 = Mutator::run (req, nullptr);
    int unbakedSent = 0;
    for (const auto& s : sent)
        unbakedSent += s.find (formatPatchAsReply (unbaked)) != std::string::npos ? 1 : 0;
    ok = check (refused.hadTweaks && ! refused.baked && refused.note.find ("weren't included") != std::string::npos
                    && refused.patch.voice == branchy.voice && refused.patch.tweaks.empty() && unbakedSent == 4
                    && mr2.passed() == 4,
                "merge mutate: a refused bake sends the unbaked code (%d/4) with a note (\"%s\")", unbakedSent,
                refused.note.c_str())
       && ok;

    // No tweaks: the patch goes as it is.
    const Patch plain = builtin ("reese");
    const MutateBase none = prepareMutateBase (plain);
    ok = check (! none.hadTweaks && ! none.baked && none.note.empty() && none.patch == plain,
                "merge mutate: a patch without tweaks is sent unchanged") && ok;
    return ok;
}

// Discard / the Original card: the processor reloads mutateOriginal (a
// PatchSnapshot) like Undo; the variation in between has no tweaks.
bool smokeDiscardTweaks()
{
    const Patch shaped = shapedReese();
    PatchSnapshot original;
    original.patch = shaped;
    original.macros.fill (0.3f);
    original.nodes = probeTouchedNodes (shaped, 48000.0);
    original.nodesKnown = true;
    const PatchSnapshot kept = original;  // mutateOriginal is a copy, never edited

    OverrideTable table, want;
    applyPatchTweaks (shaped, want);
    applyPatchTweaks (shaped, table);
    const Patch variation = prepareMutateBase (shaped).patch;  // what an audition loads
    const int duringAudition = applyPatchTweaks (variation, table);
    OverrideTable empty;
    const bool auditionClean = sameTables (table, empty);
    const auto back = parsePatch (serializePatch (kept.patch));
    applyPatchTweaks (back.patch, table);
    return check (duringAudition == 0 && auditionClean && back.ok() && tweaksBitExact (back.patch, shaped)
                      && kept.sameAs (original) && sameTables (table, want),
                  "merge discard: auditioning a variation clears the table; Discard (or the Original card) "
                  "reloads the original's tweaks and maps bit-exactly");
}

} // namespace

bool smokeMerge()
{
    bool ok = smokeBuiltinNodeIds();
    ok = smokeUndoTweaks() && ok;
    ok = smokeChangeKeepsTweaks() && ok;
    ok = smokeMutateBakes() && ok;
    ok = smokeDiscardTweaks() && ok;
    return ok;
}

} // namespace vibesynth_cli
