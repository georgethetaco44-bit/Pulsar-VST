#pragma once

// M8 settings (SPEC §6.3, §6.5), restyled for M9 as an overlay: a dimmed
// backdrop over the editor and a card with the API key (masked), the
// endpoint and the model IDs. Save writes the settings file; Esc, Close or
// a click on the backdrop closes it. "Test key" sends one tiny request with
// the typed key (or the environment key) and shows "Key works" or the
// mapped error under the key field.

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "ui/Theme.h"

namespace vibesynth::ui {

class SettingsPanel final : public juce::Component
{
public:
    explicit SettingsPanel (VibeSynthAudioProcessor& p);

    void open();
    void close();
    // The processor's key test result (called from the editor's refresh).
    void setKeyTest (const VibeSynthAudioProcessor::KeyTest& result);
    // Snapshot tool: open with a made-up key and a faked test result.
    void showForSnapshot (const juce::String& fakeKey, const VibeSynthAudioProcessor::KeyTest& result);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    std::function<void()> onClose;

private:
    void save();
    void testKey();
    juce::Rectangle<int> cardBounds() const;

    VibeSynthAudioProcessor& processor;
    juce::TextEditor keyEditor, endpointEditor, generateEditor, mutateEditor, retryEditor;
    juce::TextButton saveButton { "Save" }, closeButton { "Close" }, testButton { "Test key" };
    VibeSynthAudioProcessor::KeyTest keyTest;
    juce::Rectangle<int> keyResultArea;
    juce::String envNote;
    juce::Rectangle<int> fieldLabels[5];
};

} // namespace vibesynth::ui
