// M14e review snapshots. See SnapshotM14e.h.

#include "SnapshotM14e.h"

#include "PluginEditor.h"
#include "ai/MutateView.h"
#include "ui/AuditionStrip.h"
#include "ui/PromptPanel.h"

namespace vibesynth::ui {

void runM14eSnapshots (VibeSynthAudioProcessorEditor& editor,
                       const std::function<void (const juce::String&)>& shoot,
                       const std::function<void (double)>& play)
{
    using State = MutateSlotView::State;

    editor.showPage (0);
    editor.setKeyOverride (true);

    // 1. Takes selector in PromptPanel
    editor.setTakesForSnapshot (3);
    editor.setPromptText ("hollow square wave riddim bass with a tight 1/8 filter wobble");
    play (0.1);
    shoot ("takes_selector");

    // 2. The strip after a 3-take Generate, with one failed card
    MutateView v;
    v.open = true;
    v.numSlots = 3;
    v.auditioning = 0;
    v.baseName = "Reese";
    v.slots[0] = { State::Ready, "Literal", "Riddim Square", "Ready", "" };
    v.slots[1] = { State::Failed, "Alt source", "", "Compile failed: line 7: unknown identifier 'flt_wobble'", "line 7: unknown identifier 'flt_wobble'" };
    v.slots[2] = { State::Ready, "Alt motion", "Wobble Pulse", "Ready", "" };
    editor.setMutateOverride (v);
    play (0.1);
    shoot ("strip_3_takes_one_failed");
    editor.setMutateOverride (std::nullopt);

    // Reset prompt and takes
    editor.setTakesForSnapshot (1);
    editor.setPromptText ("");
}

} // namespace vibesynth::ui
