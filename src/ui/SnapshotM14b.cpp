// M14b review snapshots. See SnapshotM14b.h.

#include "SnapshotM14b.h"

#include "PluginEditor.h"
#include "ai/MutateView.h"
#include "engine/Lexicon.h"
#include "ui/AuditionStrip.h"
#include "ui/MacroPanel.h"
#include "ui/MutatePopover.h"
#include "ui/NodeInspector.h"
#include "ui/Theme.h"

namespace vibesynth::ui {

void runM14bSnapshots (VibeSynthAudioProcessorEditor& editor,
                       const std::function<void (const juce::String&)>& shoot,
                       const std::function<void (double)>& play)
{
    using State = MutateSlotView::State;

    editor.showPage (0);
    editor.setKeyOverride (true);

    // 1. Mutate popover open anchored to Mutate button
    editor.openMutatePopover();
    play (0.1);
    shoot ("mutate_popover");
    editor.closeMutatePopover();

    // 2. Strip with 2 variations (N=2, 3 cards total)
    MutateView v2;
    v2.open = true;
    v2.numSlots = 2;
    v2.auditioning = 0;
    v2.slots[0] = { State::Ready, "more metallic… · subtle", "Chrome Reese", "Ready", "" };
    v2.slots[1] = { State::Ready, "more metallic… · bold", "Acid Reese", "Ready", "" };
    editor.setMutateOverride (v2);
    play (0.1);
    shoot ("strip_2_variations");

    // 3. Strip with 5 variations (N=5, 6 cards total)
    MutateView v5;
    v5.open = true;
    v5.numSlots = 5;
    v5.auditioning = 0;
    v5.slots[0] = { State::Ready, "Darker", "Abyssal Reese", "Ready", "" };
    v5.slots[1] = { State::Ready, "Movement", "Liquid Reese", "Ready", "" };
    v5.slots[2] = { State::Ready, "Brighter", "Glass Reese", "Ready", "" };
    v5.slots[3] = { State::Ready, "Weirder", "Lorenz Throat", "Ready", "" };
    v5.slots[4] = { State::Ready, "Space", "Cosmic Reese", "Ready", "" };
    editor.setMutateOverride (v5);
    play (0.1);
    shoot ("strip_5_variations");

    // 4. Card showing "Saved: …"
    editor.showStripCardSavedForSnapshot (0, "Abyssal Reese");
    play (0.1);
    shoot ("strip_card_saved");
    editor.setMutateOverride (std::nullopt);

    // 5. Type value editor on a macro knob
    editor.macroPanel().openTypeValueEditorForSnapshot (0);
    play (0.1);
    shoot ("type_value_macro");
    editor.macroPanel().closeTypeValueEditorForSnapshot();

    // 6. Type value editor on an inspector knob
    const NodeRef filter { RackKind::Voice, ModuleId::FltLp, 0 };
    editor.openInspector (filter);
    editor.nodeInspector().openTypeValueEditorForSnapshot (0);
    play (0.1);
    shoot ("type_value_inspector");
    editor.nodeInspector().closeTypeValueEditorForSnapshot();
    editor.closeInspector();
}

} // namespace vibesynth::ui
