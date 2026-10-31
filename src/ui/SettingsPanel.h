#pragma once

// M8 settings (SPEC §6.3, §6.5), restyled for M9 as an overlay: a dimmed
// backdrop over the editor and a card with the API key (masked), the
// endpoint and the model IDs. Save writes the settings file; Esc, Close or
// a click on the backdrop closes it.

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui/Theme.h"

class VibeSynthAudioProcessor;

namespace vibesynth::ui {

class SettingsPanel final : public juce::Component
{
public:
    explicit SettingsPanel (VibeSynthAudioProcessor& p);

    void open();
    void close();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    std::function<void()> onClose;

private:
    void save();
    juce::Rectangle<int> cardBounds() const;

    VibeSynthAudioProcessor& processor;
    juce::TextEditor keyEditor, endpointEditor, generateEditor, mutateEditor, retryEditor;
    juce::TextButton saveButton { "Save" }, closeButton { "Close" };
    juce::String envNote;
    juce::Rectangle<int> fieldLabels[5];
};

} // namespace vibesynth::ui
