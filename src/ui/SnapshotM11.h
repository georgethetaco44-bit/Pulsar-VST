#pragma once

// M11 review snapshots (PULSAR_SNAPSHOT, see Snapshot.h), run by the
// SnapshotDriver after its own editor shots, before the node check. The
// audition strip and the inspector share the bottom page area:
//
//   inspector_change.png          the inspector on Reese's flt_lp:0 with
//                                 the Change box enabled and text typed
//   inspector_change_running.png  the same while that change runs (header
//                                 busy, box locked, "Changing..." hint)
//   inspector_change_075.png / _150.png   the first at 0.75x and 1.5x
//   audition.png                  Mutate opened over the inspector: the
//                                 strip takes its place mid-run (one ready
//                                 variation playing, one failed, one
//                                 testing on a retry, one waiting)
//   audition_hint.png             a node clicked while the strip is open:
//                                 "Keep or Discard the variation first"
//   audition_done.png             the strip after the run (3 ready, 1 failed)
//   audition_075.png / _150.png   the strip at 0.75x and 1.5x
//
// Then an end-to-end Undo check in the real processor (no network): Reese
// with odd macro values, a cutoff offset, a bypass and a macro map; load a
// preset (the tweaks clear), Undo, and compare the patch, its tweaks and
// macro maps, all 8 macro values and the override table bit for bit. It
// prints "m11check:" lines.

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

class VibeSynthAudioProcessorEditor;

namespace vibesynth::ui {

// `shoot` writes <name>.png; `play` renders `seconds` of the held note so
// the meters and traces move.
void runM11Snapshots (VibeSynthAudioProcessorEditor& editor, const std::function<void (const juce::String&)>& shoot,
                      const std::function<void (double seconds)>& play);

} // namespace vibesynth::ui
