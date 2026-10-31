// M9 macro panel. See MacroPanel.h.

#include "ui/MacroPanel.h"

#include "PluginProcessor.h"
#include "patch/LexiconLint.h"
#include "patch/PatchTweaks.h"
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
    slider.onValueChange = [this]
    {
        if (hasMod)
            slider.setModulation (true, modSource, modDepth, liveMod);
        repaint();
    };
    slider.onRightClick = [this] { showModMenu(); };
    slider.onModDepthChanged = [this] (ModSource src, double d)
    {
        modDepth = d;
        if (onAssignMod)
            onAssignMod (src, d);
    };
    addAndMakeVisible (slider);
    setPatchMacro ({}, 0.5);
}

void MacroKnob::setPatchMacro (const juce::String& newLabel, double defaultValue, bool isUsed,
                               const juce::String& drives, bool codeReads)
{
    used = isUsed;
    label = newLabel.isNotEmpty() ? newLabel : "Macro " + juce::String (index + 1);
    slider.setDoubleClickReturnValue (true, defaultValue);
    const auto host = "Macro " + juce::String (index + 1);
    juce::String mapped;
    if (drives.isNotEmpty())
        mapped = (codeReads ? "\nThe patch code reads it, and it also drives " : "\nDrives ") + drives + ".";
    juce::String modTip;
    if (hasMod)
        modTip = "\nModulated by " + juce::String (modSourceLabel (modSource))
               + " (" + (modDepth >= 0.0 ? "+" : "") + juce::String (modDepth, 2)
               + "). Drag outer ring to adjust depth; right-click to remove.";
    slider.setTooltip (used ? label + mapped + modTip + "\nHost parameter: " + host
                                  + ". Double-click resets to the patch default (" + juce::String (defaultValue, 2) + ")."
                            : "Unused by this patch" + modTip + "\nHost parameter: " + host + ".");
    slider.setDescription (used ? label : "Unused by this patch");
    slider.setAlpha ((used || hasMod) ? 1.0f : 0.3f);
    repaint();
}

void MacroKnob::setModAssignment (const ModAssignment* mod, double liveValue)
{
    hasMod = (mod != nullptr && std::abs (mod->depth) >= 1.0e-6);
    if (hasMod)
    {
        modSource = mod->source;
        modDepth = mod->depth;
        liveMod = juce::jlimit (0.0, 1.0, liveValue);
    }
    slider.setModulation (hasMod, modSource, modDepth, hasMod ? liveMod : -1.0);
    slider.setAlpha ((used || hasMod) ? 1.0f : 0.3f);
    repaint();
}

void MacroKnob::setLiveModValue (double liveValue)
{
    if (! hasMod)
        return;
    liveMod = juce::jlimit (0.0, 1.0, liveValue);
    slider.setModulation (true, modSource, modDepth, liveMod);
}

void MacroKnob::showModMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader ("Modulate " + (label.isNotEmpty() ? label : "Macro " + juce::String (index + 1)));
    static const char* kMenuItems[] = {
        "Modulate with LFO 1",
        "Modulate with LFO 2",
        "Modulate with Env 1",
        "Modulate with Env 2"
    };
    for (int i = 0; i < kNumModSources; ++i)
        m.addItem (100 + i, kMenuItems[i], true, hasMod && static_cast<int> (modSource) == i);
    if (hasMod)
    {
        m.addSeparator();
        m.addItem (110, "Remove modulation");
    }
    auto* top = getTopLevelComponent();
    if (auto* editor = findParentComponentOfClass<juce::AudioProcessorEditor>())
        top = editor;
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&slider).withParentComponent (top),
                     [safe = juce::Component::SafePointer<MacroKnob> (this)] (int result)
                     {
                         if (safe == nullptr || result == 0)
                             return;
                         if (result >= 100 && result < 100 + kNumModSources)
                         {
                             const auto src = static_cast<ModSource> (result - 100);
                             const double d = safe->hasMod ? safe->modDepth : 0.5;
                             if (safe->onAssignMod)
                                 safe->onAssignMod (src, d);
                         }
                         else if (result == 110 && safe->onRemoveMod)
                         {
                             safe->onRemoveMod();
                         }
                     });
}

bool MacroKnob::isInterestedInDragSource (const SourceDetails& details)
{
    return details.description.toString().startsWith ("mod:");
}

void MacroKnob::itemDragEnter (const SourceDetails&)
{
    dragHover = true;
    repaint();
}

void MacroKnob::itemDragExit (const SourceDetails&)
{
    dragHover = false;
    repaint();
}

void MacroKnob::itemDropped (const SourceDetails& details)
{
    dragHover = false;
    const juce::String desc = details.description.toString();
    const int srcIdx = desc.fromFirstOccurrenceOf ("mod:", false, false).getIntValue();
    if (srcIdx >= 0 && srcIdx < kNumModSources && onAssignMod)
    {
        const double d = hasMod ? modDepth : 0.5;
        onAssignMod (static_cast<ModSource> (srcIdx), d);
    }
    repaint();
}

void MacroKnob::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        showModMenu();
}

namespace {
constexpr int kKnobSize = 40;
constexpr int kKnobBlock = kKnobSize + 1 + 13 + 12; // knob, gap, label, value = 66 px
} // namespace

void MacroKnob::paint (juce::Graphics& g)
{
    using namespace Theme;
    if (dragHover)
    {
        g.setColour (accent.withAlpha (0.35f));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f, 1.5f);
    }
    if (! used && ! hasMod)
        return; // dimmed knob only: no label, no value
    auto r = getLocalBounds();
    r.removeFromTop (slider.getBottom() + 1);
    g.setColour (text);
    g.setFont (font (smallSize, Weight::Medium));
    g.drawFittedText (label, r.removeFromTop (13), juce::Justification::centred, 1, 0.85f);
    g.setColour (hasMod ? modSourceColour (modSource) : dimText);
    g.setFont (font (captionSize));
    g.drawText (juce::String (slider.getValue(), 2), r.removeFromTop (12), juce::Justification::centred);
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
MacroPanel::MacroPanel (juce::AudioProcessorValueTreeState& s, VibeSynthAudioProcessor* proc)
    : state (s),
      processor (proc),
      voiceMode (*s.getParameter (VibeSynthAudioProcessor::voiceModeParamId))
{
    for (int i = 0; i < 8; ++i)
    {
        knobs[(size_t) i] = std::make_unique<MacroKnob> (state, i);
        knobs[(size_t) i]->onAssignMod = [this, i] (ModSource src, double depth)
        {
            if (processor != nullptr)
                processor->setMacroMod (src, i + 1, depth);
        };
        knobs[(size_t) i]->onRemoveMod = [this, i]
        {
            if (processor != nullptr)
                processor->clearMacroMod (i + 1);
        };
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
                           const std::array<juce::String, 8>& labels, const std::array<double, 8>& defaults,
                           unsigned readMask)
{
    patchName = name;
    patchMode = mode;
    for (int i = 0; i < 8; ++i)
        knobs[(size_t) i]->setPatchMacro (labels[(size_t) i], defaults[(size_t) i], ((readMask >> i) & 1u) != 0u);
    voiceMode.setPatchMode (mode);
    updateGlide();
    repaint();
}

void MacroPanel::showPatch (const vibesynth::Patch& patch)
{
    const unsigned read = vibesynth::macrosRead (patch);
    std::array<juce::String, 8> labels, drives;
    for (int i = 0; i < 8; ++i)
        labels[(size_t) i] = i < (int) patch.macros.size() ? juce::String (patch.macros[(size_t) i].label)
                                                           : juce::String();
    for (const auto& m : patch.macroMaps)
    {
        const int i = m.macro - 1;
        if (i < 0 || i >= 8)
            continue;
        vibesynth::NodeRef n;
        const vibesynth::LexiconEntry* e = vibesynth::parseNodeId (m.node, n)
                                               ? vibesynth::primaryLexiconEntry ((int) n.module) : nullptr;
        const int k = e != nullptr ? vibesynth::lexiconArgIndex (*e, m.arg.c_str()) : -1;
        const juce::String argLabel = k >= 0 ? juce::String (e->args[k].label) : juce::String (m.arg);
        auto& d = drives[(size_t) i];
        d += (d.isEmpty() ? "" : ", ") + juce::String (m.node) + " " + argLabel;
        // An unused macro is named after the first argument it drives.
        if (((read >> i) & 1u) == 0u && labels[(size_t) i].isEmpty())
            labels[(size_t) i] = argLabel;
    }
    patchName = juce::String (patch.name);
    patchMode = juce::String (patch.voiceMode);
    for (int i = 0; i < 8; ++i)
    {
        const bool codeReads = ((read >> i) & 1u) != 0u;
        const auto* mod = vibesynth::findMacroMod (patch, i + 1);
        const double curVal = knobs[(size_t) i]->slider.getValue();
        const double liveVal = mod != nullptr ? juce::jlimit (0.0, 1.0, curVal + mod->depth * 0.5) : curVal;
        knobs[(size_t) i]->setModAssignment (mod, liveVal);
        knobs[(size_t) i]->setPatchMacro (labels[(size_t) i], patch.macroDefault (i),
                                          codeReads || drives[(size_t) i].isNotEmpty() || mod != nullptr,
                                          drives[(size_t) i], codeReads);
    }
    voiceMode.setPatchMode (patchMode);
    updateGlide();
    repaint();
}

void MacroPanel::tick (const OverrideTable& overrides)
{
    for (int i = 0; i < 8; ++i)
    {
        if (knobs[(size_t) i]->slider.hasModulation())
        {
            const float live = overrides.liveMacroValue[i].load (std::memory_order_relaxed);
            knobs[(size_t) i]->setLiveModValue (static_cast<double> (live));
        }
    }
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
    g.fillRect ((float) grid * 2.0f, (float) voiceLabelArea.getY() - 5.0f,
                r.getWidth() - (float) grid * 4.0f, hairline);
}

void MacroPanel::resized()
{
    using namespace Theme;
    auto r = getLocalBounds().reduced (grid * 2, grid);
    titleArea = r.removeFromTop (20);
    r.removeFromTop (1);

    auto voiceRow = r.removeFromBottom (22);
    r.removeFromBottom (6);
    const int cellW = r.getWidth() / 4;
    const int cellH = r.getHeight() / 2;
    for (int i = 0; i < 8; ++i)
        knobs[(size_t) i]->setBounds (r.getX() + (i % 4) * cellW, r.getY() + (i / 4) * cellH, cellW, cellH);

    voiceLabelArea = voiceRow; // the hairline above it
    voiceMode.setBounds (voiceRow.removeFromLeft (grid * 18).reduced (0, 1));
    voiceRow.removeFromLeft (grid * 2);
    glideLabelArea = voiceRow.removeFromLeft (grid * 5);
    glideValueArea = voiceRow.removeFromRight (grid * 6);
    voiceRow.removeFromRight (grid);
    glide.setBounds (voiceRow);
}

} // namespace vibesynth::ui
