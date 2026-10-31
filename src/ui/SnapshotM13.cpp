// M13 review snapshots. See SnapshotM13.h.

#include "ui/SnapshotM13.h"

#include "PluginEditor.h"
#include "patch/PatchTweaks.h"

namespace vibesynth::ui {

void runM13Snapshots (VibeSynthAudioProcessorEditor& editor,
                      const std::function<void (const juce::String&)>& shoot,
                      const std::function<void (double)>& play)
{
    const int w = Theme::baseWidth, h = Theme::baseHeight;
    auto& p = editor.getProcessor();

    // Reset Reese macros and tweaks cleanly for the M13 shots.
    p.setNodeArg ("flt_lp:0", "cutoff", 0, 0.0);
    p.clearNodeMacroMap ("flt_lp:0", "res");
    p.setNodeBypass ("fx_fold#0", false);
    const auto& reese = p.getUiPatch();
    for (int i = 0; i < VibeSynthAudioProcessor::numMacros; ++i)
        p.apvts.getParameter ("macro" + juce::String (i + 1))
            ->setValueNotifyingHost (static_cast<float> (reese.macroDefault (i)));

    editor.closeInspector();
    editor.closeModPopover();
    editor.setSize (w, h);
    editor.showPage (0);
    editor.setKeyOverride (true);

    // 1. The strip on Reese (with LFO 2 set to Saw Up 1/8T for visual variety).
    ModLfoSpec lfo2;
    lfo2.shape = LfoShape::SawUp;
    lfo2.sync = true;
    lfo2.syncDiv = LfoSyncDiv::Div1_8T;
    lfo2.retrigger = true;
    p.setModLfo (1, lfo2);
    editor.setKeyOverride (true); // refresh() synchronously (no message loop between shots)
    play (0.25);
    shoot ("mod_strip");

    // 2. An LFO popover open (LFO 1 settings).
    editor.openModPopover (ModSource::Lfo1);
    play (0.15);
    shoot ("mod_lfo_popover");
    editor.closeModPopover();

    // 3. A macro with a depth ring (Macro 1 modulated by LFO 1, Macro 2 by Env 1).
    p.setMacroMod (ModSource::Lfo1, 1, 0.55);
    p.setMacroMod (ModSource::Env1, 2, -0.45);
    editor.setKeyOverride (true);
    play (0.25);
    shoot ("mod_macro_ring");

    // 4. An inspector knob with modulation (flt_lp:0 cutoff + LFO 1, res + Env 1).
    p.setNodeArg ("flt_lp:0", "cutoff", ArgTweak::kOffset, 0.10);
    p.setNodeArgMod (ModSource::Lfo1, "flt_lp:0", "cutoff", 0.50);
    p.setNodeArgMod (ModSource::Env1, "flt_lp:0", "res", 0.40);
    editor.setKeyOverride (true);
    editor.openInspector (NodeRef { RackKind::Voice, ModuleId::FltLp, 0 });
    play (0.3);
    shoot ("mod_inspector_knob");

    // 5. At 0.75x and 1.5x scale.
    editor.setSize (w * 3 / 4, h * 3 / 4);
    play (0.15);
    shoot ("mod_075");

    editor.setSize (w * 3 / 2, h * 3 / 2);
    play (0.15);
    shoot ("mod_150");

    // Clean up state after M13 shots.
    editor.setSize (w, h);
    editor.closeInspector();
    p.clearMacroMod (1);
    p.clearMacroMod (2);
    p.clearNodeArgMod ("flt_lp:0", "cutoff");
    p.clearNodeArgMod ("flt_lp:0", "res");
    p.setNodeArg ("flt_lp:0", "cutoff", 0, 0.0);
    p.setModLfo (1, ModLfoSpec {});
    editor.setKeyOverride (true);
}

} // namespace vibesynth::ui
