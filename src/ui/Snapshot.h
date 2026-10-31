#pragma once

// M9 design review snapshots (hidden, Standalone only). With
// PULSAR_SNAPSHOT=<dir> set, the editor loads Reese, waits for its node
// list, then renders itself to PNGs in <dir> and quits the app:
//
//   default.png        Reese with a held note (an offline render of the
//                      patch feeds the meters, so the nodes glow)
//   default@2x.png     the same at 2x pixel density (HiDPI check)
//   generating.png     a fake generation in progress
//   error.png          a fake generation error, reason expanded
//   nokey.png          no API key: the inline "Add your Meta API key" card
//   settings_key.png   Settings after a successful Test key (fake key)
//   settings_key_error.png  Settings after a rejected key (the mapped 401)
//   code.png           the Code tab
//   presets.png        the Presets tab
//   history.png        the History tab
//   scale_075.png      720 x 480
//   scale_150.png      1440 x 960
//   nodes/<preset>.png the node view of each built-in, with a printed line
//                      per preset (font size, cut-off or overlapping pills)
//
// No network is used and the audio device is not needed: the note is
// rendered on the message thread with its own PatchProgram and
// PatchPlayer, and only its meter peaks go into the processor's bank.

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

class VibeSynthAudioProcessorEditor;

namespace vibesynth {
class PatchProgram;
class PatchPlayer;
}

namespace vibesynth::ui {

class SnapshotDriver final : private juce::Timer
{
public:
    // Null unless this is the Standalone and PULSAR_SNAPSHOT is set.
    static std::unique_ptr<SnapshotDriver> createIfRequested (VibeSynthAudioProcessorEditor& editor);

    SnapshotDriver (VibeSynthAudioProcessorEditor& editor, juce::File dir);
    ~SnapshotDriver() override;

private:
    void timerCallback() override;
    void playNote (double seconds);
    void shoot (const juce::String& name, float pixelScale = 1.0f);
    void runShots();
    void checkNodeViews();

    VibeSynthAudioProcessorEditor& editor;
    juce::File dir;
    int waited = 0;
    int startPatchVersion = 0;
    bool noteStarted = false;
    std::unique_ptr<PatchProgram> program;
    std::unique_ptr<PatchPlayer> player;
};

} // namespace vibesynth::ui
