// M9 settings overlay. See SettingsPanel.h.

#include "ui/SettingsPanel.h"

#include "PluginProcessor.h"
#include "ui/PulsarLookAndFeel.h"

namespace vibesynth::ui {

namespace {
const char* kLabels[5] = { "API key", "Endpoint", "Generate model", "Mutate model", "Retry model" };
} // namespace

SettingsPanel::SettingsPanel (VibeSynthAudioProcessor& p) : processor (p)
{
    keyEditor.setPasswordCharacter ((juce::juce_wchar) 0x2022);
    keyEditor.setTooltip ("Stored only in the Pulsar settings file on this computer");
    endpointEditor.setTooltip ("Chat-completions endpoint");
    generateEditor.setTooltip ("Model for Generate");
    mutateEditor.setTooltip ("Model for Mutate (later milestone)");
    retryEditor.setTooltip ("Model for self-heal retries");
    for (auto* e : { &keyEditor, &endpointEditor, &generateEditor, &mutateEditor, &retryEditor })
    {
        e->setFont (Theme::font (13.0f));
        e->setIndents (10, 0);
        e->setJustification (juce::Justification::centredLeft);
        e->setColour (juce::TextEditor::backgroundColourId, Theme::background);
        e->onEscapeKey = [this] { close(); };
        e->onReturnKey = [this] { save(); };
        addAndMakeVisible (e);
    }
    setButtonStyle (saveButton, ButtonStyle::Primary);
    setButtonStyle (closeButton, ButtonStyle::Ghost);
    setButtonStyle (testButton, ButtonStyle::Ghost);
    testButton.setTooltip ("Send one tiny request to check the key, endpoint and model");
    testButton.onClick = [this] { testKey(); };
    addAndMakeVisible (testButton);
    keyEditor.onTextChange = [this]
    {
        // The result belonged to the old text.
        processor.clearKeyTest();
        setKeyTest (processor.getKeyTest());
    };
    saveButton.setTooltip ("Save the settings file");
    closeButton.setTooltip ("Close without saving (Esc)");
    saveButton.onClick = [this] { save(); };
    closeButton.onClick = [this] { close(); };
    addAndMakeVisible (saveButton);
    addAndMakeVisible (closeButton);
    setWantsKeyboardFocus (true);
}

void SettingsPanel::open()
{
    auto& s = processor.aiSettings();
    const auto& c = s.config();
    processor.clearKeyTest();
    setKeyTest (processor.getKeyTest());
    keyEditor.setText (juce::String (s.savedKey()), false);
    endpointEditor.setText (juce::String (c.endpoint), false);
    generateEditor.setText (juce::String (c.generateModel), false);
    mutateEditor.setText (juce::String (c.mutateModel), false);
    retryEditor.setText (juce::String (c.retryModel), false);
    envNote = s.keyFromEnvironment() ? "Using VIBESYNTH_API_KEY from the environment (it overrides the saved key)."
                                     : "Saved in " + juce::String (s.filePath());
    setVisible (true);
    toFront (true);
    keyEditor.grabKeyboardFocus();
}

void SettingsPanel::showForSnapshot (const juce::String& fakeKey, const VibeSynthAudioProcessor::KeyTest& result)
{
    open();
    keyEditor.setText (fakeKey, false);
    setKeyTest (result);
}

void SettingsPanel::setKeyTest (const VibeSynthAudioProcessor::KeyTest& result)
{
    if (result == keyTest)
        return;
    keyTest = result;
    testButton.setEnabled (keyTest.state != VibeSynthAudioProcessor::KeyTest::State::Running);
    repaint (keyResultArea.expanded (0, Theme::grid));
}

void SettingsPanel::testKey()
{
    // The typed key, else the environment's; the typed endpoint and model.
    auto typed = keyEditor.getText().trim().toStdString();
    if (typed.empty() && processor.aiSettings().keyFromEnvironment())
        typed = processor.aiSettings().apiKey();
    auto c = processor.aiSettings().config();
    const vibesynth::AiConfig defaults;
    auto field = [] (const juce::TextEditor& e, const std::string& fallback)
    {
        const auto t = e.getText().trim();
        return t.isEmpty() ? fallback : t.toStdString();
    };
    c.endpoint = field (endpointEditor, defaults.endpoint);
    c.generateModel = field (generateEditor, defaults.generateModel);
    processor.testApiKey (typed, c);
    setKeyTest (processor.getKeyTest());
}

void SettingsPanel::close()
{
    setVisible (false);
    if (onClose)
        onClose();
}

void SettingsPanel::save()
{
    auto& s = processor.aiSettings();
    auto c = s.config();
    auto field = [] (const juce::TextEditor& e, const std::string& fallback)
    {
        const auto t = e.getText().trim();
        return t.isEmpty() ? fallback : t.toStdString();
    };
    const vibesynth::AiConfig defaults;
    c.endpoint = field (endpointEditor, defaults.endpoint);
    c.generateModel = field (generateEditor, defaults.generateModel);
    c.mutateModel = field (mutateEditor, defaults.mutateModel);
    c.retryModel = field (retryEditor, defaults.retryModel);
    s.setConfig (c);
    s.setSavedKey (keyEditor.getText().trim().toStdString());
    const bool ok = s.save();
    using Kind = VibeSynthAudioProcessor::StatusKind;
    processor.setStatus (ok ? "Settings saved" : "Could not save settings to " + juce::String (s.filePath()),
                         ok ? Kind::Info : Kind::Error);
    if (ok)
        close();
}

juce::Rectangle<int> SettingsPanel::cardBounds() const
{
    return getLocalBounds().withSizeKeepingCentre (Theme::grid * 68, Theme::grid * 47);
}

void SettingsPanel::paint (juce::Graphics& g)
{
    using namespace Theme;
    g.fillAll (background.withAlpha (0.78f));
    const auto card = cardBounds();
    g.setColour (panel);
    g.fillRoundedRectangle (card.toFloat(), radius + 2.0f);
    g.setColour (border);
    g.drawRoundedRectangle (card.toFloat().reduced (0.5f), radius + 2.0f, hairline);

    auto r = card.reduced (grid * 3);
    g.setColour (text);
    g.setFont (font (17.0f, Weight::SemiBold));
    g.drawText ("Settings", r.removeFromTop (grid * 3), juce::Justification::centredLeft);
    g.setColour (dimText);
    g.setFont (font (smallSize));
    g.drawText ("Generation service and models. Nothing is sent until you press Generate.",
                r.removeFromTop (grid * 3), juce::Justification::centredLeft);

    g.setFont (font (smallSize, Weight::Medium));
    for (int i = 0; i < 5; ++i)
        g.drawText (kLabels[i], fieldLabels[i], juce::Justification::centredLeft);

    using State = VibeSynthAudioProcessor::KeyTest::State;
    if (keyTest.state != State::Idle)
    {
        const auto ink = keyTest.state == State::Ok ? accent : keyTest.state == State::Failed ? warning : dimText;
        auto line = keyResultArea;
        g.setColour (ink);
        g.fillEllipse ((float) line.getX() + 1.0f, (float) line.getCentreY() - 3.0f, 6.0f, 6.0f);
        g.setFont (font (smallSize, Weight::Medium));
        g.drawFittedText (keyTest.text, line.withTrimmedLeft (grid * 2), juce::Justification::centredLeft, 1, 1.0f);
    }

    g.setColour (dimText);
    g.setFont (font (captionSize));
    g.drawFittedText (envNote, saveButton.getBounds().withLeft (card.getX() + grid * 3).withRight (closeButton.getX() - grid * 2),
                      juce::Justification::centredLeft, 2);
}

void SettingsPanel::resized()
{
    using namespace Theme;
    auto r = cardBounds().reduced (grid * 3);
    r.removeFromTop (grid * 7);
    juce::TextEditor* fields[] = { &keyEditor, &endpointEditor, &generateEditor, &mutateEditor, &retryEditor };
    for (int i = 0; i < 5; ++i)
    {
        auto row = r.removeFromTop (grid * 4);
        fieldLabels[i] = row.removeFromLeft (grid * 16);
        if (i == 0)
        {
            testButton.setBounds (row.removeFromRight (grid * 11));
            row.removeFromRight (grid);
        }
        fields[i]->setBounds (row);
        if (i == 0)
        {
            // The key test's result, under the key field.
            keyResultArea = r.removeFromTop (grid * 3).withTrimmedLeft (grid * 16).withTrimmedTop (grid / 2);
            continue;
        }
        r.removeFromTop (grid);
    }
    auto buttons = r.removeFromBottom (grid * 4);
    saveButton.setBounds (buttons.removeFromRight (grid * 12));
    buttons.removeFromRight (grid);
    closeButton.setBounds (buttons.removeFromRight (grid * 12));
}

void SettingsPanel::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasClicked() && ! cardBounds().contains (e.getPosition()))
        close();
}

bool SettingsPanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        close();
        return true;
    }
    return false;
}

} // namespace vibesynth::ui
