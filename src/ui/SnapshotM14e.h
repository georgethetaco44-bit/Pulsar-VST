#pragma once

// M14e review snapshots (SPEC §9.7, §13 item 14e). Called from SnapshotDriver::runShots:
//   - takes_selector: PromptPanel showing the Takes selector (1 2 3 4)
//   - strip_3_takes_one_failed: Audition strip after a 3-take Generate with one failed card

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

class VibeSynthAudioProcessorEditor;

namespace vibesynth::ui {

void runM14eSnapshots (VibeSynthAudioProcessorEditor& editor,
                       const std::function<void (const juce::String&)>& shoot,
                       const std::function<void (double)>& play);

} // namespace vibesynth::ui
