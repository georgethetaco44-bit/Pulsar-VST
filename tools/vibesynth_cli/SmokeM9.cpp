// M9 smoketests. See SmokeM9.h.

#include "SmokeM9.h"

#include <juce_core/juce_core.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "CliCheck.h"
#include "engine/NodeMeters.h"
#include "patch/PatchLibrary.h"
#include "patch/PatchPlayer.h"
#include "patch/PatchValidator.h"
#include "ui/NodeLayout.h"

namespace vibesynth_cli {

namespace {

using namespace vibesynth;

constexpr double kRate = 48000.0;

bool subsetOf (const TouchedNodes& a, const TouchedNodes& b)
{
    for (int m = 0; m < kNumModules; ++m)
        if ((a.voice[m] & ~b.voice[m]) != 0u || (a.master[m] & ~b.master[m]) != 0u)
            return false;
    return true;
}

struct Ev
{
    int samplePosition;
    const uint8_t* data;
    int numBytes;
};

} // namespace

// The validator lists every touched (module, slot); the compile-only probe
// finds the same set (or a subset) for every built-in.
bool smokeTouchedNodes()
{
    bool ok = true;
    int equal = 0;
    for (int i = 0; i < numBuiltinPresets(); ++i)
    {
        const auto parsed = parsePatch (builtinPresetText (i));
        if (! parsed.ok())
        {
            ok = check (false, "M9 nodes %s parses: %s", builtinPresetFile (i), parsed.error.c_str());
            continue;
        }
        const auto vr = validatePatch (parsed.patch, kRate);
        const auto probe = probeTouchedNodes (parsed.patch, kRate);
        const bool sub = subsetOf (probe, vr.touched);
        ok = check (vr.ok() && vr.touched.any() && probe.any() && sub,
                    "M9 nodes %-22s validator %2d, probe %2d%s%s%s", builtinPresetFile (i), vr.touched.count(),
                    probe.count(), sub ? (probe == vr.touched ? "" : " (subset)") : " NOT A SUBSET",
                    vr.ok() ? "" : "; validator: ", vr.error.c_str())
           && ok;
        equal += probe == vr.touched ? 1 : 0;
    }
    std::printf ("[INFO] M9 probe == validator node list for %d of %d built-ins\n", equal, numBuiltinPresets());

    // A cancelled probe returns nothing.
    const auto reese = parsePatch (builtinPresetText (0)).patch;
    std::atomic<bool> cancel { true };
    ok = check (! probeTouchedNodes (reese, kRate, &cancel).any(), "M9 a cancelled probe returns an empty node list") && ok;
    return ok;
}

// Holding a note publishes only touched nodes into the bank, the rack
// meters are cleared by each harvest, and a bank take resets the cell.
bool smokeMeterHarvest()
{
    const auto patch = parsePatch (builtinPresetText (0)).patch;
    const auto vr = validatePatch (patch, kRate);
    const auto& touched = vr.touched;
    if (! vr.ok())
        return check (false, "M9 harvest: Reese passes the offline test (%s)", vr.error.c_str());
    std::string error;
    auto program = PatchProgram::build (patch, kRate, error);
    if (program == nullptr)
        return check (false, "M9 harvest: Reese compiles (%s)", error.c_str());

    float targets[PatchPlayer::kNumMacros];
    for (int m = 0; m < PatchPlayer::kNumMacros; ++m)
        targets[m] = (float) patch.macroDefault (m);
    PatchPlayer player;
    player.setProgram (program.get());
    player.prepare (kRate, 256, targets);
    const uint8_t on[3] = { 0x90, 40, 100 };
    const std::vector<Ev> events = { { 0, on, 3 } };
    auto it = events.cbegin();
    NodeMeterBank bank;
    for (int start = 0; start < 9600; start += 256)
        player.renderChunk (256, targets, it, events.cend(), start);
    player.harvestMeters (bank);

    int lit = 0, stray = 0;
    for (int m = 0; m < kNumModules; ++m)
        for (int s = 0; s < kMaxSlotsPerModule; ++s)
        {
            if (bank.voice[m][s].load() > 0.0f)
                ((touched.voice[m] >> s) & 1u) ? ++lit : ++stray;
            if (bank.master[m][s].load() > 0.0f && ((touched.master[m] >> s) & 1u) == 0u)
                ++stray;
        }
    bool ok = check (lit > 0 && stray == 0, "M9 harvest: %d voice nodes lit while a note is held, %d outside the node list",
                     lit, stray);

    // The racks were cleared: a second harvest with no audio in between
    // publishes nothing.
    NodeMeterBank second;
    player.harvestMeters (second);
    int again = 0;
    for (int m = 0; m < kNumModules; ++m)
        for (int s = 0; s < kMaxSlotsPerModule; ++s)
            again += second.voice[m][s].load() > 0.0f ? 1 : 0;
    ok = check (again == 0, "M9 harvest clears the rack meters (%d left)", again) && ok;

    int cell = -1;
    for (int m = 0; m < kNumModules && cell < 0; ++m)
        for (int s = 0; s < kMaxSlotsPerModule && cell < 0; ++s)
            if (bank.voice[m][s].load() > 0.0f)
                cell = m * kMaxSlotsPerModule + s;
    if (cell >= 0)
    {
        auto& c = bank.voice[cell / kMaxSlotsPerModule][cell % kMaxSlotsPerModule];
        const float v = NodeMeterBank::take (c);
        NodeMeterBank::raise (c, v * 0.5f);
        NodeMeterBank::raise (c, v * 0.25f);
        ok = check (v > 0.0f && NodeMeterBank::take (c) == v * 0.5f && c.load() == 0.0f,
                    "M9 bank: raise keeps the max, take resets") && ok;
    }
    return ok;
}

bool smokeNodeLayout()
{
    bool ok = true;
    ModuleId m = ModuleId::Count;
    ok = check (moduleFromName ("util_xover_hi", m) && m == ModuleId::UtilXover && moduleFromName ("BUS_REVERB_R", m)
                    && m == ModuleId::BusReverb && ! moduleFromName ("fx_fold", m)
                    && std::string (moduleShortName (ModuleId::FltLp)) == "flt_lp",
                "M9 module names and aliases")
       && ok;

    // Reese: the touched set from the validator and its own graph.edges.
    const auto patch = parsePatch (builtinPresetText (0)).patch;
    const auto vr = validatePatch (patch, kRate);
    const auto& touched = vr.touched;
    ok = check (vr.ok(), "M9 layout: Reese passes the offline test%s%s", vr.ok() ? "" : ": ", vr.error.c_str()) && ok;
    std::vector<std::pair<std::string, std::string>> edges;
    const auto graph = juce::JSON::parse (juce::String (patch.graphJson));
    if (const auto* list = graph.getProperty ("edges", {}).getArray())
        for (const auto& e : *list)
            if (const auto* pair = e.getArray(); pair != nullptr && pair->size() == 2)
                edges.emplace_back ((*pair)[0].toString().toStdString(), (*pair)[1].toString().toStdString());

    const auto a = layoutNodeGraph (touched, edges);
    const auto b = layoutNodeGraph (touched, edges);
    bool same = a.nodes.size() == b.nodes.size() && a.edges.size() == b.edges.size();
    for (size_t i = 0; same && i < a.nodes.size(); ++i)
        same = a.nodes[i].layer == b.nodes[i].layer && a.nodes[i].row == b.nodes[i].row && a.nodes[i].loose == b.nodes[i].loose;
    ok = check (same && (int) a.nodes.size() == touched.count(), "M9 layout is deterministic (%d nodes, %d edges)",
                (int) a.nodes.size(), (int) a.edges.size())
       && ok;

    auto find = [&a] (ModuleId mod, int slot)
    {
        for (int i = 0; i < (int) a.nodes.size(); ++i)
            if (! a.nodes[(size_t) i].master && a.nodes[(size_t) i].module == mod && a.nodes[(size_t) i].slot == slot)
                return i;
        return -1;
    };
    const int lp = find (ModuleId::FltLp, 0), fb = find (ModuleId::UtilSafeFb, 0), saw = find (ModuleId::OscSupersaw, 0);
    bool contracted = false, feedback = false;
    for (const auto& e : a.edges)
    {
        contracted = contracted || (e.from == lp && e.to == fb);           // flt_lp -> fx_sat -> fx_fold -> util_safe_fb
        feedback = feedback || (e.feedback && ((e.from == fb && e.to == saw) || (e.from == saw && e.to == lp)
                                                || (e.from == lp && e.to == fb)));
    }
    ok = check (lp >= 0 && fb >= 0 && saw >= 0 && contracted && feedback,
                "M9 layout: edges contract through non-nodes; the safe_fb loop is marked feedback")
       && ok;

    // A touched node no edge mentions goes in the loose row; master nodes
    // are their own region.
    TouchedNodes t;
    t.voice[(int) ModuleId::OscSaw] = 1u;
    t.voice[(int) ModuleId::FltLp] = 1u;
    t.voice[(int) ModuleId::ChaosLorenz] = 2u;
    t.master[(int) ModuleId::BusReverb] = 1u;
    const auto c = layoutNodeGraph (t, { { "osc_saw:0", "flt_lp:0" }, { "flt_lp:0", "out" }, { "out", "bus_reverb" },
                                         { "made_up:0", "flt_lp:0" } });
    int loose = 0, cross = 0;
    for (const auto& n : c.nodes)
        loose += n.loose ? 1 : 0;
    for (const auto& e : c.edges)
        cross += e.crossRegion ? 1 : 0;
    ok = check (c.nodes.size() == 4 && loose == 1 && c.loose[0] == 1 && c.layers[0] == 2 && cross == 1,
                "M9 layout: loose row (%d), voice layers %d, cross-region edges %d", loose, c.layers[0], cross)
       && ok;
    return ok;
}

bool smokePatchLibrary()
{
    bool ok = true;
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getNonexistentChildFile ("pulsar_m9_smoke", "", false);
    const auto history = dir.getChildFile ("Generated");
    auto patch = parsePatch (builtinPresetText (0)).patch;
    const juce::Time t0 (2026, 9, 30, 12, 0, 0, 0, true);
    juce::String error;
    for (int i = 0; i < 55; ++i)
    {
        patch.prompt = "prompt " + std::to_string (i);
        PatchLibrary::addToHistory (history, patch, t0 + juce::RelativeTime::seconds (i), error);
    }
    const auto entries = PatchLibrary::listHistory (history);
    const int files = history.getNumberOfChildFiles (juce::File::findFiles, "*.pulsar");
    ok = check (files == PatchLibrary::kHistoryLimit && entries.size() == 50 && entries.front().prompt == "prompt 54"
                    && entries.back().prompt == "prompt 5" && entries.front().time == t0 + juce::RelativeTime::seconds (54),
                "M9 history keeps the newest %d (%d files, newest '%s')", PatchLibrary::kHistoryLimit, files,
                entries.empty() ? "" : entries.front().prompt.toRawUTF8())
       && ok;

    const auto user = dir.getChildFile ("Patches");
    const auto f1 = PatchLibrary::saveUserPatch (user, "My Reese", patch, false, error);
    const auto f2 = PatchLibrary::saveUserPatch (user, "My Reese", patch, false, error);
    const auto f3 = PatchLibrary::saveUserPatch (user, "My Reese", patch, true, error);
    const auto reread = parsePatch (f3.loadFileAsString().toStdString());
    const auto listed = PatchLibrary::listPatches (user);
    ok = check (f1.existsAsFile() && f2 == juce::File() && f3 == f1 && reread.ok() && reread.patch.name == "My Reese"
                    && listed.size() == 1,
                "M9 user save: refuses to overwrite unless asked, sets the name")
       && ok;
    dir.deleteRecursively();
    return ok;
}

} // namespace vibesynth_cli
