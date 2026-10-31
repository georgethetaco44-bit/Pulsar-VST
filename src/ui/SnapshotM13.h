#pragma once

// M13 review snapshots (SPEC §9.8). Called from SnapshotDriver::runShots.

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

class VibeSynthAudioProcessorEditor;

namespace vibesynth::ui {

void runM13Snapshots (VibeSynthAudioProcessorEditor& editor,
                      const std::function<void (const juce::String&)>& shoot,
                      const std::function<void (double)>& play);

} // namespace vibesynth::ui
