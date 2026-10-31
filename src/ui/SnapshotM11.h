#pragma once

// M11 review snapshots (PULSAR_SNAPSHOT, see Snapshot.h), run by the
// SnapshotDriver after its own editor shots, before the node check:
//
//   audition.png       the audition strip mid-run: one ready variation
//                      playing, one failed (amber, mapped reason), one
//                      testing on a retry, one waiting
//   audition_done.png  the strip after the run (3 ready, 1 failed)
//   change_popup.png   "Change this node" opened on Reese's flt_lp:0
//
// Then an end-to-end Undo check in the real processor (no network): odd
// macro values on Reese, load a preset, Undo, and compare the patch and
// all 8 macro values bit for bit. It prints one "m11check:" line.

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

class VibeSynthAudioProcessorEditor;

namespace vibesynth::ui {

void runM11Snapshots (VibeSynthAudioProcessorEditor& editor, const std::function<void (const juce::String&)>& shoot);

} // namespace vibesynth::ui
