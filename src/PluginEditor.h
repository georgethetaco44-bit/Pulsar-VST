#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <optional>

#include "PluginProcessor.h"
#include "ui/CodeView.h"
#include "ui/MacroPanel.h"
#include "ui/NodeView.h"
#include "ui/PresetBrowser.h"
#include "ui/PromptPanel.h"
#include "ui/PulsarLookAndFeel.h"
#include "ui/SettingsPanel.h"

namespace vibesynth::ui { class SnapshotDriver; }

//==============================================================================
/**
    The Pulsar editor (M9, SPEC §9.5, §9.6). Dark, one cyan accent
    (ui/Theme.h). Laid out once at 960 x 640 on an 8 px grid inside
    `content`, which is scaled as a whole (fixed aspect, 0.75x to 1.5x), so
    everything stays vector and HiDPI-clean.

      header   wordmark, prompt (Enter / Shift+Enter / Esc), Generate or
               Cancel, status line (stage + elapsed, result, amber error
               with the full reason on click)
      left     8 macro knobs, Voice Mode, Glide
      right    node visualizer (30 fps meters)
      bottom   Presets / Code / History tabs, output meter, settings gear

    Everything the editor shows lives in the processor, which notifies it
    through ChangeBroadcaster on the message thread; no worker calls the
    editor, so it can close and reopen at any time, even mid-generation.
    Only the node view and the output meter repaint at 30 fps (and only
    while something is audible); the rest repaints on change. Construction
    does no file I/O: the user and history lists are read when their page
    is first shown.

    PULSAR_SNAPSHOT=<dir> (Standalone only, undocumented) renders review
    PNGs and quits: see ui/Snapshot.h.
*/
class VibeSynthAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                            private juce::ChangeListener,
                                            private juce::Timer
{
public:
    explicit VibeSynthAudioProcessorEditor (VibeSynthAudioProcessor&);
    ~VibeSynthAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    // Snapshot tool hooks.
    VibeSynthAudioProcessor& getProcessor() noexcept { return processor; }
    void setHeaderOverride (std::optional<vibesynth::ui::HeaderState> state);
    void setPromptText (const juce::String& text) { header.setPrompt (text); }
    void showPage (int index);  // 0 Presets, 1 Code, 2 History
    void setErrorDetailOpen (bool open);
    // Forces "has an API key" for the header (nullopt: the real state), so
    // the snapshots don't depend on this machine's key.
    void setKeyOverride (std::optional<bool> hasKey);
    // Opens Settings with a made-up key and a faked key-test result.
    void showSettingsForSnapshot (const juce::String& fakeKey, const VibeSynthAudioProcessor::KeyTest& result);
    void closeSettings() { settings.close(); }
    vibesynth::ui::NodeView& nodeView() noexcept { return nodes; }
    void tickMeters();          // one meter frame now

    static constexpr int numPages = 3;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void refresh();
    vibesynth::ui::HeaderState headerState() const;
    void layoutContent();
    void placeErrorDetail();
    void setFocusOrder();

    VibeSynthAudioProcessor& processor;
    vibesynth::ui::PulsarLookAndFeel lookAndFeel;  // outlives every child (declared first)

    juce::Component content;
    vibesynth::ui::PromptPanel header;
    vibesynth::ui::MacroPanel macros;
    vibesynth::ui::NodeView nodes;
    std::array<juce::TextButton, numPages> tabs;
    vibesynth::ui::PresetBrowser presets;
    vibesynth::ui::CodeView code;
    vibesynth::ui::HistoryPage history;
    vibesynth::ui::OutputMeter meter;
    vibesynth::ui::GearButton gear;
    vibesynth::ui::ErrorDetail errorDetail;
    vibesynth::ui::SettingsPanel settings;
    juce::TooltipWindow tooltips { this, 600 };

    juce::Rectangle<int> tabRow, toolbarSlot, pageArea;
    int page = 0;
    bool errorOpen = false;
    int shownPatchVersion = -1;
    int shownNodesVersion = -1;
    juce::String shownSource;
    vibesynth::ui::HeaderState shownHeader;
    std::optional<vibesynth::ui::HeaderState> headerOverride;
    std::optional<bool> keyOverride;
    int ticks = 0;

    std::unique_ptr<vibesynth::ui::SnapshotDriver> snapshot;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VibeSynthAudioProcessorEditor)
};
