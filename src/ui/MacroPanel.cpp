// M9 macro panel. See MacroPanel.h.

#include "ui/MacroPanel.h"

#include "PluginProcessor.h"
#include "ui/PulsarLookAndFeel.h"

namespace vibesynth::ui {

namespace {
constexpr float kKnobStart = juce::MathConstants<float>::pi * 1.25f;
constexpr float kKnobEnd = juce::MathConstants<float>::pi * 2.75f;

juce::String glideText (double g)
{
    const double ms = g * g * 1000.0; // SPEC 9.1: glide^2 seconds
    if (ms < 0.5)
        return "Off";
    return ms < 999.5 ? juce::String (juce::roundToInt (ms)) + " ms" : juce::String (ms / 1000.0, 2) + " s";
}
} // namespace

//==============================================================================
MacroKnob::MacroKnob (juce::AudioProcessorValueTreeState& state, int i) : index (i)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.setRotaryParameters (kKnobStart, kKnobEnd, true);
    slider.setMouseDragSensitivity (200);
    slider.setVelocityBasedMode (false);
    slider.setScrollWheelEnabled (true);
    slider.setMouseClickGrabsKeyboardFocus (false);
    slider.setWantsKeyboardFocus (true);
    slider.setTitle ("Macro " + juce::String (i + 1));
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        state, "macro" + juce::String (i + 1), slider);
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider);
    setPatchMacro ({}, 0.5);
}

void MacroKnob::setPatchMacro (const juce::String& newLabel, double defaultValue)
{
    label = newLabel.isNotEmpty() ? newLabel : "Macro " + juce::String (index + 1);
    slider.setDoubleClickReturnValue (true, defaultValue);
    const auto host = "Macro " + juce::String (index + 1);
    slider.setTooltip (label + "\nHost parameter: " + host + ". Double-click resets to the patch default ("
                       + juce::String (defaultValue, 2) + ").");
    slider.setDescription (label);
    repaint();
}

namespace {
constexpr int kKnobSize = 48;
constexpr int kKnobBlock = kKnobSize + 4 + 16 + 14; // knob, gap, label, value
} // namespace

void MacroKnob::paint (juce::Graphics& g)
{
    using namespace Theme;
    auto r = getLocalBounds();
    r.removeFromTop (slider.getBottom() + 4);
    g.setColour (text);
    g.setFont (font (smallSize, Weight::Medium));
    g.drawFittedText (label, r.removeFromTop (16), juce::Justification::centred, 1, 0.85f);
    g.setColour (dimText);
    g.setFont (font (captionSize));
    g.drawText (juce::String (slider.getValue(), 2), r.removeFromTop (14), juce::Justification::centred);
}

void MacroKnob::resized()
{
    // The knob, its label and value as one block, centred in the cell.
    const int top = juce::jmax (0, (getHeight() - kKnobBlock) / 2);
    slider.setBounds ((getWidth() - kKnobSize) / 2, top, kKnobSize, kKnobSize);
}

//==============================================================================
VoiceModeControl::VoiceModeControl (juce::RangedAudioParameter& param)
    : attachment (param, [this] (float v) { update (juce::roundToInt (v)); })
{
    static const char* names[] = { "Patch", "Poly", "Mono" };
    for (int i = 0; i < 3; ++i)
    {
        auto& b = buttons[(size_t) i];
        b.setButtonText (names[i]);
        setButtonStyle (b, ButtonStyle::Segment);
        b.setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0)
                             | (i < 2 ? juce::Button::ConnectedOnRight : 0));
        b.setClickingTogglesState (false);
        b.onClick = [this, i] { attachment.setValueAsCompleteGesture ((float) i); };
        addAndMakeVisible (b);
    }
    buttons[1].setTooltip ("Voice Mode: Poly, 8 voices");
    buttons[2].setTooltip ("Voice Mode: Mono, legato with glide");
    attachment.sendInitialUpdate();
}

void VoiceModeControl::setPatchMode (const juce::String& m)
{
    buttons[0].setTooltip ("Voice Mode: follow the patch (" + (m.isNotEmpty() ? m : juce::String ("poly")) + ")");
}

void VoiceModeControl::update (int newMode)
{
    mode = juce::jlimit (0, 2, newMode);
    for (int i = 0; i < 3; ++i)
        buttons[(size_t) i].setToggleState (i == mode, juce::dontSendNotification);
    if (onChange)
        onChange();
}

void VoiceModeControl::resized()
{
    auto r = getLocalBounds();
    const int w = r.getWidth() / 3;
    for (int i = 0; i < 3; ++i)
        buttons[(size_t) i].setBounds (i < 2 ? r.removeFromLeft (w) : r);
}

//==============================================================================
MacroPanel::MacroPanel (juce::AudioProcessorValueTreeState& s)
    : state (s),
      voiceMode (*s.getParameter (VibeSynthAudioProcessor::voiceModeParamId))
{
    for (int i = 0; i < 8; ++i)
    {
        knobs[(size_t) i] = std::make_unique<MacroKnob> (state, i);
        addAndMakeVisible (*knobs[(size_t) i]);
    }
    voiceMode.onChange = [this] { updateGlide(); };
    addAndMakeVisible (voiceMode);

    glide.setSliderStyle (juce::Slider::LinearHorizontal);
    glide.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    glide.setMouseClickGrabsKeyboardFocus (false);
    glide.setWantsKeyboardFocus (true);
    glide.setTitle ("Glide");
    glideAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        state, VibeSynthAudioProcessor::glideParamId, glide);
    glide.onValueChange = [this] { updateGlide(); };
    addAndMakeVisible (glide);
    updateGlide();
}

void MacroPanel::setPatch (const juce::String& name, const juce::String& mode,
                           const std::array<juce::String, 8>& labels, const std::array<double, 8>& defaults)
{
    patchName = name;
    patchMode = mode;
    for (int i = 0; i < 8; ++i)
        knobs[(size_t) i]->setPatchMacro (labels[(size_t) i], defaults[(size_t) i]);
    voiceMode.setPatchMode (mode);
    updateGlide();
    repaint();
}

void MacroPanel::updateGlide()
{
    // Glide only acts in mono (SPEC 9.1).
    const bool mono = voiceMode.getMode() == 2 || (voiceMode.getMode() == 0 && patchMode == "mono");
    glide.setAlpha (mono ? 1.0f : 0.45f);
    glide.setTooltip ("Glide: " + glideText (glide.getValue())
                      + (mono ? juce::String (" (mono, legato notes)") : juce::String (" (used in mono only)")));
    repaint (glideValueArea);
}

juce::Array<juce::Component*> MacroPanel::focusOrder()
{
    juce::Array<juce::Component*> order;
    for (auto& k : knobs)
        order.add (&k->slider);
    for (auto& b : voiceMode.buttons)
        order.add (&b);
    order.add (&glide);
    return order;
}

void MacroPanel::paint (juce::Graphics& g)
{
    using namespace Theme;
    const auto r = getLocalBounds().toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (r, radius);
    g.setColour (border);
    g.drawRoundedRectangle (r.reduced (0.5f), radius, hairline);

    auto title = titleArea;
    g.setColour (dimText);
    g.setFont (font (captionSize, Weight::SemiBold).withExtraKerningFactor (0.12f));
    g.drawText ("MACROS", title.removeFromRight (grid * 9), juce::Justification::centredRight);
    g.setColour (text);
    g.setFont (font (15.0f, Weight::SemiBold));
    g.drawFittedText (patchName, title, juce::Justification::centredLeft, 1, 1.0f);

    g.setColour (dimText);
    g.setFont (font (smallSize, Weight::Medium));
    g.drawText ("Glide", glideLabelArea, juce::Justification::centredLeft);
    g.setColour (text);
    g.drawText (glideText (glide.getValue()), glideValueArea, juce::Justification::centredLeft);

    // Hairline between the macros and the voice controls.
    g.setColour (border);
    g.fillRect ((float) grid * 2.0f, (float) voiceLabelArea.getY() - (float) grid - 0.5f,
                r.getWidth() - (float) grid * 4.0f, hairline);
}

void MacroPanel::resized()
{
    using namespace Theme;
    auto r = getLocalBounds().reduced (grid * 2);
    titleArea = r.removeFromTop (grid * 3);
    r.removeFromTop (grid);

    auto voiceRow = r.removeFromBottom (grid * 4);
    r.removeFromBottom (grid * 2);
    const int cellW = r.getWidth() / 4;
    const int cellH = r.getHeight() / 2;
    for (int i = 0; i < 8; ++i)
        knobs[(size_t) i]->setBounds (r.getX() + (i % 4) * cellW, r.getY() + (i / 4) * cellH, cellW, cellH);

    voiceLabelArea = voiceRow; // the hairline above it
    voiceMode.setBounds (voiceRow.removeFromLeft (grid * 18).reduced (0, 2));
    voiceRow.removeFromLeft (grid * 2);
    glideLabelArea = voiceRow.removeFromLeft (grid * 5);
    glideValueArea = voiceRow.removeFromRight (grid * 6);
    voiceRow.removeFromRight (grid);
    glide.setBounds (voiceRow);
}

} // namespace vibesynth::ui
