// M11 review snapshots. See SnapshotM11.h.

#include "ui/SnapshotM11.h"

#include <cstdio>
#include <cstring>

#include "PluginEditor.h"
#include "patch/PatchTweaks.h"

namespace vibesynth::ui {

namespace {

MutateView fakeView (bool finished)
{
    using State = MutateSlotView::State;
    MutateView v;
    v.open = true;
    v.running = ! finished;
    v.baseName = "Reese";
    v.startedMs = juce::Time::getMillisecondCounterHiRes() - 9400.0;
    v.auditioning = finished ? 2 : 0;
    const char* dirs[] = { "Darker", "Movement", "Brighter", "Weirder" };
    for (int i = 0; i < MutateView::kSlots; ++i)
        v.slots[(size_t) i].direction = dirs[i];
    v.slots[0] = { State::Ready, "Darker", "Abyss Reese", "Ready", "" };
    v.slots[1] = { State::Failed, "Movement",  "", "too loud: peak +14.2 dBFS (max +12) at note 28, macros all 1",
                   "too loud: peak +14.2 dBFS (max +12) at note 28, macros all 1 (after 2 retries)" };
    if (finished)
    {
        v.slots[2] = { State::Ready, "Brighter", "Glass Reese", "Ready", "" };
        v.slots[3] = { State::Ready, "Weirder", "Lorenz Throat", "Ready", "" };
    }
    else
    {
        v.slots[2] = { State::Testing, "Brighter", "", "Testing (retry 1/2)", "" };
        v.slots[3] = { State::Waiting, "Weirder", "", "Waiting for model", "" };
    }
    return v;
}

bool sameBits (double a, double b)
{
    return std::memcmp (&a, &b, sizeof (double)) == 0;
}

bool sameTweakBits (const Patch& a, const Patch& b)
{
    if (a.tweaks.size() != b.tweaks.size() || a.macroMaps.size() != b.macroMaps.size())
        return false;
    for (size_t i = 0; i < a.tweaks.size(); ++i)
    {
        if (! (a.tweaks[i] == b.tweaks[i]) || a.tweaks[i].args.size() != b.tweaks[i].args.size())
            return false;
        for (size_t k = 0; k < a.tweaks[i].args.size(); ++k)
            if (! sameBits (a.tweaks[i].args[k].value, b.tweaks[i].args[k].value))
                return false;
    }
    for (size_t i = 0; i < a.macroMaps.size(); ++i)
        if (! (a.macroMaps[i] == b.macroMaps[i]) || ! sameBits (a.macroMaps[i].min, b.macroMaps[i].min)
            || ! sameBits (a.macroMaps[i].max, b.macroMaps[i].max))
            return false;
    return true;
}

} // namespace

void runM11Snapshots (VibeSynthAudioProcessorEditor& editor, const std::function<void (const juce::String&)>& shoot,
                      const std::function<void (double)>& play)
{
    const int w = Theme::baseWidth, h = Theme::baseHeight;
    const NodeRef filter { RackKind::Voice, ModuleId::FltLp, 0 };
    editor.showPage (0);
    editor.setKeyOverride (true);

    // "Change this node" in the inspector (right-click opens it on the box).
    editor.clickNode (filter, true);
    editor.nodeInspector().setChangeText ("make this filter wobble at 1/4 notes");
    play (0.3);
    shoot ("inspector_change");
    editor.setSize (w * 3 / 4, h * 3 / 4);
    play (0.1);
    shoot ("inspector_change_075");
    editor.setSize (w * 3 / 2, h * 3 / 2);
    play (0.1);
    shoot ("inspector_change_150");
    editor.setSize (w, h);

    HeaderState changing;
    changing.generating = true;
    changing.canGenerate = true;
    changing.canUndo = true;
    changing.kind = HeaderState::Kind::Busy;
    changing.stage = "Waiting for model";
    changing.seconds = 6;
    editor.setHeaderOverride (changing);
    editor.setChangeRunningOverride (true);
    play (0.1);
    shoot ("inspector_change_running");
    editor.setChangeRunningOverride (std::nullopt);
    editor.setHeaderOverride (std::nullopt);
    editor.nodeInspector().clearChangeText();

    // Mutate opened with the inspector up: the strip takes its place.
    const bool inspectorWasOpen = editor.nodeInspector().isVisible();
    editor.setMutateOverride (fakeView (false));
    const bool replaced = inspectorWasOpen && ! editor.nodeInspector().isVisible();
    play (0.1);
    shoot ("audition");
    editor.clickNode (filter, false);  // the inspector stays shut
    const bool stayedShut = ! editor.nodeInspector().isVisible();
    play (0.1);
    shoot ("audition_hint");
    editor.setMutateOverride (fakeView (true));
    shoot ("audition_done");
    editor.setSize (w * 3 / 4, h * 3 / 4);
    play (0.1);
    shoot ("audition_075");
    editor.setSize (w * 3 / 2, h * 3 / 2);
    play (0.1);
    shoot ("audition_150");
    editor.setSize (w, h);
    std::printf ("m11check: the strip replaced the inspector: %s; a node click while it is open kept the "
                 "inspector shut: %s\n",
                 replaced ? "yes" : "NO", stayedShut ? "yes" : "NO");
    editor.setMutateOverride (std::nullopt);

    // End-to-end Undo: patch and exact macro values after a preset load.
    auto& p = editor.getProcessor();
    auto waitFor = [&p] (const std::function<bool()>& done)
    {
        for (int i = 0; i < 500 && ! done(); ++i)
        {
            juce::Thread::sleep (10);
            p.deliverPendingResults();
        }
        return done();
    };
    auto macros = [&p]
    {
        std::array<float, VibeSynthAudioProcessor::numMacros> m {};
        for (int i = 0; i < VibeSynthAudioProcessor::numMacros; ++i)
            m[(size_t) i] = p.apvts.getRawParameterValue ("macro" + juce::String (i + 1))->load();
        return m;
    };
    const float odd[] = { 0.1234567f, 0.9876543f, 0.000001f, 1.0f, 0.3333333f, 0.7071068f, 0.5f, 0.0f };
    for (int i = 0; i < VibeSynthAudioProcessor::numMacros; ++i)
        p.apvts.getParameter ("macro" + juce::String (i + 1))->setValueNotifyingHost (odd[i]);
    // M10 merge: tweaks and a macro map ride along (knob edits don't push).
    const int depthBeforeTweaks = p.getUndoDepth();
    p.setNodeArg ("flt_lp:0", "cutoff", ArgTweak::kOffset, 0.123456789);
    p.setNodeBypass ("fx_fold#0", true);
    p.setNodeMacroMap ("flt_lp:0", "res", 5, 0.1, 0.65);
    const bool editsPushed = p.getUndoDepth() != depthBeforeTweaks;
    const vibesynth::Patch before = p.getUiPatch();
    auto tableHas = [&p]
    {
        const auto& t = p.nodeOverrides();
        const auto& f = t.nodes[0][(int) ModuleId::FltLp][0];
        return t.nodes[0][(int) ModuleId::FxFold][0].bypass.load() != 0
            && f.args[2].mode.load() == (uint8_t) ArgMode::Offset && f.args[3].mode.load() == (uint8_t) ArgMode::Macro
            && f.args[3].macro.load() == 4;
    };
    const bool tableBefore = tableHas();
    const auto beforeMacros = macros();
    const int depth0 = p.canUndo() ? 1 : 0;
    int v = p.getPatchVersion();
    p.loadBuiltin (2);
    const bool loaded = waitFor ([&] { return p.getPatchVersion() != v; });
    const bool changed = p.getUiPatch().voice != before.voice && macros() != beforeMacros;
    const bool cleared = p.getUiPatch().tweaks.empty() && p.getUiPatch().macroMaps.empty() && ! tableHas();
    v = p.getPatchVersion();
    p.undo();
    const bool undone = waitFor ([&] { return p.getPatchVersion() != v; });
    const auto afterMacros = macros();
    const bool samePatch = p.getUiPatch() == before && sameTweakBits (p.getUiPatch(), before) && before.tweaks.size() == 2
                        && before.macroMaps.size() == 1;
    const bool sameTable = tableHas();
    const bool sameMacros = std::memcmp (afterMacros.data(), beforeMacros.data(), sizeof (float) * afterMacros.size()) == 0;
    const bool all = loaded && changed && cleared && undone && samePatch && sameMacros && tableBefore && sameTable
                  && ! editsPushed;
    std::printf ("m11check: undo after a preset load restored '%s' with its 2 tweaked nodes, its macro map, "
                 "the override table and all 8 macro values bit-exactly: %s (loaded %d, changed %d, tweaks "
                 "cleared by the load %d, undone %d, patch+tweaks %d, macros %d, table %d/%d, knob edits pushed %d, "
                 "stack had entries %d)\n",
                 before.name.c_str(), all ? "yes" : "NO", loaded ? 1 : 0, changed ? 1 : 0, cleared ? 1 : 0,
                 undone ? 1 : 0, samePatch ? 1 : 0, sameMacros ? 1 : 0, tableBefore ? 1 : 0, sameTable ? 1 : 0,
                 editsPushed ? 1 : 0, depth0);
    std::fflush (stdout);
}

} // namespace vibesynth::ui
