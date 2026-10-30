#include "PluginEditor.h"

VibeSynthAudioProcessorEditor::VibeSynthAudioProcessorEditor (VibeSynthAudioProcessor& p)
    : AudioProcessorEditor (p), processor (p)
{
    for (int i = 0; i < processor.getNumPrograms(); ++i)
        presetBox.addItem (processor.getProgramName (i), i + 1);
    presetBox.setTextWhenNothingSelected ("Preset");
    presetBox.onChange = [this]
    {
        const int index = presetBox.getSelectedId() - 1;
        if (index >= 0)
            processor.loadBuiltin (index);
    };
    loadButton.onClick = [this] { loadClicked(); };
    saveButton.onClick = [this] { saveClicked(); };

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    labelsLine.setJustificationType (juce::Justification::centredLeft);
    labelsLine.setFont (juce::FontOptions (13.0f));

    addAndMakeVisible (presetBox);
    addAndMakeVisible (loadButton);
    addAndMakeVisible (saveButton);
    addAndMakeVisible (statusLabel);
    addAndMakeVisible (labelsLine);
    addAndMakeVisible (generic);

    processor.addChangeListener (this);
    refresh();
    setSize (juce::jmax (460, generic.getWidth()), headerHeight + generic.getHeight());
}

VibeSynthAudioProcessorEditor::~VibeSynthAudioProcessorEditor()
{
    processor.removeChangeListener (this);
}

void VibeSynthAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void VibeSynthAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();
    auto header = area.removeFromTop (headerHeight).reduced (8, 6);

    auto row = header.removeFromTop (28);
    saveButton.setBounds (row.removeFromRight (80));
    row.removeFromRight (6);
    loadButton.setBounds (row.removeFromRight (80));
    row.removeFromRight (6);
    presetBox.setBounds (row);

    header.removeFromTop (4);
    statusLabel.setBounds (header.removeFromTop (24));
    labelsLine.setBounds (header.removeFromTop (24));
    generic.setBounds (area);
}

void VibeSynthAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh();
}

void VibeSynthAudioProcessorEditor::refresh()
{
    presetBox.setSelectedId (processor.getCurrentProgram() + 1, juce::dontSendNotification);
    statusLabel.setText (processor.getStatusText(), juce::dontSendNotification);

    juce::StringArray labels;
    for (int i = 0; i < VibeSynthAudioProcessor::numMacros; ++i)
        labels.add (juce::String (i + 1) + " " + processor.getMacroLabel (i));
    labelsLine.setText ("Macros: " + labels.joinIntoString ("  |  "), juce::dontSendNotification);
}

void VibeSynthAudioProcessorEditor::loadClicked()
{
    chooser = std::make_unique<juce::FileChooser> (
        "Load a Pulsar patch",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
        "*.pulsar");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file.existsAsFile())
                                  processor.loadPatchFile (file);
                          });
}

void VibeSynthAudioProcessorEditor::saveClicked()
{
    const auto name = juce::File::createLegalFileName (processor.getActivePatchName());
    chooser = std::make_unique<juce::FileChooser> (
        "Save the Pulsar patch",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
            .getChildFile (name + ".pulsar"),
        "*.pulsar");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();
                              if (file == juce::File())
                                  return;
                              file = file.withFileExtension ("pulsar");
                              const bool ok = file.replaceWithText (juce::String (processor.getActivePatchText()));
                              processor.setStatus (ok ? "Saved " + file.getFileName()
                                                      : "Could not save " + file.getFullPathName());
                          });
}
