#pragma once

// M14b review snapshots (SPEC §9.6, §9.7). Called from SnapshotDriver::runShots:
//   - mutate_popover: Popover open anchored to Mutate button
//   - strip_2_variations: Audition strip with 2 variations
//   - strip_5_variations: Audition strip with 5 variations
//   - strip_card_saved: Card showing "Saved: …"
//   - type_value_macro: Type value editor open on a macro knob
//   - type_value_inspector: Type value editor open on an inspector knob

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

class VibeSynthAudioProcessorEditor;

namespace vibesynth::ui {

void runM14bSnapshots (VibeSynthAudioProcessorEditor& editor,
                       const std::function<void (const juce::String&)>& shoot,
                       const std::function<void (double)>& play);

} // namespace vibesynth::ui
