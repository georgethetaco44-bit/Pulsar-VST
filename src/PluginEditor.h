#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

#include "PluginProcessor.h"

//==============================================================================
/**
    Temporary M5 editor (the real UI is M9, SPEC 9.5): built-in preset box,
    Load... / Save... (.pulsar), a status line (load result or reject
    reason), the patch's macro labels, and JUCE's generic editor below for
    the macro sliders.
*/
class VibeSynthAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                            private juce::ChangeListener
{
public:
    explicit VibeSynthAudioProcessorEditor (VibeSynthAudioProcessor&);
    ~VibeSynthAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refresh();
    void loadClicked();
    void saveClicked();

    VibeSynthAudioProcessor& processor;
    juce::ComboBox presetBox;
    juce::TextButton loadButton { "Load..." };
    juce::TextButton saveButton { "Save..." };
    juce::Label statusLabel;
    juce::Label labelsLine;
    juce::GenericAudioProcessorEditor generic { processor };
    std::unique_ptr<juce::FileChooser> chooser;

    static constexpr int headerHeight = 96;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VibeSynthAudioProcessorEditor)
};
