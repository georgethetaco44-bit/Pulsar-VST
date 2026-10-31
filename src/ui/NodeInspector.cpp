// M10 node inspector. See NodeInspector.h.

#include "ui/NodeInspector.h"

#include <cmath>
#include <complex>

#include "PluginProcessor.h"
#include "patch/LexiconLint.h"
#include "ui/PulsarLookAndFeel.h"

namespace vibesynth::ui {

namespace {

constexpr float kKnobStart = juce::MathConstants<float>::pi * 1.25f;
constexpr float kKnobEnd = juce::MathConstants<float>::pi * 2.75f;
constexpr double kHzOffsetRange = 24.0;   // semitones either way
constexpr int kKnobSize = 50;

juce::String hzText (double hz)
{
    if (hz >= 1000.0)
        return juce::String (hz / 1000.0, hz >= 10000.0 ? 1 : 2) + " kHz";
    return juce::String (hz, hz < 10.0 ? 2 : (hz < 100.0 ? 1 : 0)) + " Hz";
}

void caps (juce::Graphics& g, const juce::String& s, juce::Rectangle<int> r, juce::Justification j)
{
    g.setFont (Theme::font (Theme::captionSize, Theme::Weight::SemiBold).withExtraKerningFactor (0.12f));
    g.drawText (s, r, j);
}

bool isFilter (ModuleId m)
{
    return m == ModuleId::FltLp || m == ModuleId::FltHp || m == ModuleId::FltBp || m == ModuleId::FltLadder;
}

const char* regionName (RackKind r)
{
    return r == RackKind::Master ? "MASTER" : "VOICE";
}

} // namespace

//==============================================================================
void InspectorKnobLook::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                                          float startAngle, float endAngle, juce::Slider& s)
{
    using namespace Theme;
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const float d = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto c = bounds.getCentre();
    const float arcR = d * 0.5f - 3.0f;
    const float faceR = arcR - 7.0f;
    const float angle = startAngle + pos * (endAngle - startAngle);
    const float alpha = s.isEnabled() ? 1.0f : 0.45f;
    const bool bipolar = (bool) s.getProperties().getWithDefault ("bipolar", false);
    const juce::PathStrokeType arcStroke (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    juce::Path track;
    track.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
    g.setColour (border.withMultipliedAlpha (alpha));
    g.strokePath (track, arcStroke);

    // Offset knobs fill from the centre (no change) towards the value.
    const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
    if (std::abs (angle - from) > 0.01f)
    {
        juce::Path value;
        value.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
        g.setColour (accent.withMultipliedAlpha (alpha));
        g.strokePath (value, arcStroke);
    }

    const bool hot = s.isMouseOverOrDragging() && s.isEnabled();
    g.setColour (hot ? border : control);
    g.fillEllipse (c.x - faceR, c.y - faceR, faceR * 2.0f, faceR * 2.0f);
    g.setColour (border.brighter (hot ? 0.3f : 0.0f));
    g.drawEllipse (c.x - faceR, c.y - faceR, faceR * 2.0f, faceR * 2.0f, hairline);
    const auto dir = juce::Point<float> (std::sin (angle), -std::cos (angle));
    g.setColour (text.withMultipliedAlpha (alpha));
    g.drawLine (juce::Line<float> (c + dir * (faceR * 0.3f), c + dir * (faceR * 0.85f)), 2.0f);

    // The live value (what the function really gets), on the inner ring.
    const double live = (double) s.getProperties().getWithDefault ("live", -1.0);
    if (live >= 0.0)
    {
        const float a = startAngle + (float) juce::jlimit (0.0, 1.0, live) * (endAngle - startAngle);
        const float r = arcR - 4.5f;
        const juce::Point<float> p (c.x + r * std::sin (a), c.y - r * std::cos (a));
        g.setColour (text);
        g.fillEllipse (p.x - 2.0f, p.y - 2.0f, 4.0f, 4.0f);
    }
    if (s.hasKeyboardFocus (false))
    {
        g.setColour (accent.withAlpha (0.8f));
        g.drawEllipse (c.x - arcR - 3.0f, c.y - arcR - 3.0f, (arcR + 3.0f) * 2.0f, (arcR + 3.0f) * 2.0f, 1.0f);
    }
}

void InspectorKnobLook::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                                          float minPos, float maxPos, juce::Slider::SliderStyle style,
                                          juce::Slider& s)
{
    using namespace Theme;
    const float cy = (float) y + (float) height * 0.5f;
    const auto track = juce::Rectangle<float> ((float) x, cy - 1.5f, (float) width, 3.0f);
    g.setColour (border);
    g.fillRoundedRectangle (track, 1.5f);
    if (style != juce::Slider::TwoValueHorizontal)
    {
        g.setColour (accent);
        g.fillRoundedRectangle (track.withRight (pos), 1.5f);
        return;
    }
    g.setColour (accent.withMultipliedAlpha (s.isEnabled() ? 1.0f : 0.4f));
    g.fillRect (juce::Rectangle<float> (minPos, cy - 1.5f, juce::jmax (1.0f, maxPos - minPos), 3.0f));
    for (const float p : { minPos, maxPos })
    {
        g.setColour (text);
        g.fillRoundedRectangle (p - 2.5f, cy - 5.0f, 5.0f, 10.0f, 1.5f);
    }
}

//==============================================================================
double filterResponseDb (ModuleId module, double cutoff, double res, double hz, double voiceRate) noexcept
{
    const double c = juce::jlimit (0.0, 1.0, cutoff);
    const double r = juce::jlimit (0.0, 1.0, res);
    const double fc = juce::jmin (20.0 * std::pow (1000.0, c), 0.45 * voiceRate);
    const std::complex<double> s (0.0, hz / fc);
    std::complex<double> h (1.0, 0.0);
    if (module == ModuleId::FltLadder)
    {
        // Filters.h: k = 4.2 res, input gain 1 + k/2, four one-poles.
        const double k = 4.2 * r;
        const auto p = (1.0 + s) * (1.0 + s) * (1.0 + s) * (1.0 + s);
        h = (1.0 + 0.5 * k) / (p + k);
    }
    else
    {
        // TPT SVF: damping k = 2 (1 - res^0.25), floored at 0.05.
        const double k = juce::jmax (0.05, 2.0 * (1.0 - std::sqrt (std::sqrt (r))));
        const auto den = s * s + k * s + 1.0;
        if (module == ModuleId::FltLp)
            h = 1.0 / den;
        else if (module == ModuleId::FltHp)
            h = s * s / den;
        else if (module == ModuleId::FltBp)
            h = s / den;
        else
            return 0.0;
    }
    return 20.0 * std::log10 (juce::jmax (1.0e-9, std::abs (h)));
}

void TraceDisplay::setData (const float* points, int n, bool isRing, bool isLive)
{
    data.assign (points, points + juce::jmax (0, n));
    ring = isRing;
    live = isLive;
    repaint();
}

void TraceDisplay::setFilter (ModuleId module, double c, double r, double voiceRate)
{
    filter = module;
    cutoff = c;
    res = r;
    rate = voiceRate;
}

void TraceDisplay::paint (juce::Graphics& g)
{
    using namespace Theme;
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (background);
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (border);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, hairline);

    auto inner = getLocalBounds().reduced (8, 6);
    auto captionRow = inner.removeFromTop (14);
    g.setColour (dimText);
    // With a filter's response on the right, the left caption stays short.
    caps (g, juce::String::fromUTF8 (ring ? "OUTPUT \xc2\xb7 LAST 2 S"
                                          : (filter != ModuleId::Count ? "OUTPUT" : "OUTPUT \xc2\xb7 2 NOTE CYCLES")),
          captionRow, juce::Justification::centredLeft);
    const auto plot = inner.reduced (0, 4).toFloat();

    // Filters: the magnitude response from the live cutoff / res, on a log
    // frequency axis 20 Hz - 20 kHz, -36..+24 dB.
    if (filter != ModuleId::Count)
    {
        const float zeroDb = plot.getY() + plot.getHeight() * (24.0f / 60.0f);
        g.setColour (border);
        g.fillRect (plot.getX(), zeroDb, plot.getWidth(), hairline);
        for (const double f : { 100.0, 1000.0, 10000.0 })
        {
            const float x = plot.getX() + plot.getWidth() * (float) (std::log10 (f / 20.0) / 3.0);
            g.fillRect (x, plot.getY(), hairline, plot.getHeight());
        }
        juce::Path curve, fill;
        const int steps = 96;
        for (int i = 0; i <= steps; ++i)
        {
            const double f = 20.0 * std::pow (1000.0, (double) i / steps);
            const double db = juce::jlimit (-36.0, 24.0, filterResponseDb (filter, cutoff, res, f, rate));
            const float x = plot.getX() + plot.getWidth() * (float) i / (float) steps;
            const float yy = plot.getY() + plot.getHeight() * (float) ((24.0 - db) / 60.0);
            if (i == 0)
            {
                curve.startNewSubPath (x, yy);
                fill.startNewSubPath (x, plot.getBottom());
            }
            else
                curve.lineTo (x, yy);
            fill.lineTo (x, yy);
        }
        fill.lineTo (plot.getRight(), plot.getBottom());
        fill.closeSubPath();
        g.setColour (accent.withAlpha (0.07f));
        g.fillPath (fill);
        g.setColour (text.withAlpha (0.55f));
        g.strokePath (curve, juce::PathStrokeType (1.25f));
        const double fc = juce::jmin (20.0 * std::pow (1000.0, juce::jlimit (0.0, 1.0, cutoff)), 0.45 * rate);
        g.setColour (dimText);
        caps (g, juce::String::fromUTF8 ("RESPONSE \xc2\xb7 ") + hzText (fc).toUpperCase(), captionRow,
              juce::Justification::centredRight);
        g.setFont (font (9.0f));
        for (const auto& [f, label] : { std::pair<double, const char*> { 100.0, "100" }, { 1000.0, "1k" }, { 10000.0, "10k" } })
        {
            const float x = plot.getX() + plot.getWidth() * (float) (std::log10 (f / 20.0) / 3.0);
            g.drawText (label, juce::Rectangle<float> (x + 2.0f, plot.getBottom() - 11.0f, 24.0f, 11.0f),
                        juce::Justification::centredLeft);
        }
    }
    else
    {
        g.setColour (border);
        const float mid = ring ? plot.getBottom() : plot.getCentreY();
        g.fillRect (plot.getX(), mid - 0.5f, plot.getWidth(), hairline);
    }

    if (data.size() < 2)
    {
        g.setColour (dimText);
        g.setFont (font (smallSize));
        g.drawText ("Play a note to see this node", plot, juce::Justification::centred);
        return;
    }
    float peak = 0.0f, lowest = 0.0f;
    for (const float v : data)
    {
        peak = juce::jmax (peak, std::abs (v));
        lowest = juce::jmin (lowest, v);
    }
    const bool unipolar = ring && lowest > -1.0e-3f;
    // Audio frames are scaled to fit (shape, not level); rolling traces
    // keep their scale up to 1 so an envelope reads as a level.
    const float scale = ring ? 1.0f / juce::jmax (1.0f, peak) : (peak > 1.0e-5f ? 0.92f / peak : 0.0f);
    juce::Path p;
    for (size_t i = 0; i < data.size(); ++i)
    {
        const float x = plot.getX() + plot.getWidth() * (float) i / (float) (data.size() - 1);
        const float v = juce::jlimit (-1.0f, 1.0f, data[i] * scale);
        const float yy = unipolar ? plot.getBottom() - v * plot.getHeight() : plot.getCentreY() - v * plot.getHeight() * 0.5f;
        if (i == 0)
            p.startNewSubPath (x, yy);
        else
            p.lineTo (x, yy);
    }
    const float a = live ? 1.0f : 0.45f;
    g.setColour (accent.withAlpha (0.18f * a));
    g.strokePath (p, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (accent.withAlpha (a));
    g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

//==============================================================================
ArgKnob::ArgKnob (const LexiconArg& m, int index) : meta (m), argIndex (index)
{
    knob.setLookAndFeel (&look);
    knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    knob.setRotaryParameters (kKnobStart, kKnobEnd, true);
    knob.setMouseDragSensitivity (220);
    knob.setScrollWheelEnabled (true);
    knob.setMouseClickGrabsKeyboardFocus (false);
    knob.setTitle (meta.label);
    knob.onValueChange = [this] { knobMoved(); };
    knob.addMouseListener (this, false);
    addAndMakeVisible (knob);

    mapRange.setLookAndFeel (&look);
    mapRange.setSliderStyle (juce::Slider::TwoValueHorizontal);
    mapRange.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    mapRange.setRange (meta.lo, meta.hi, meta.kind == ArgKind::Choice ? 1.0 : 0.0);
    if (meta.kind == ArgKind::Hz)
        mapRange.setSkewFactorFromMidPoint (std::sqrt (meta.lo * meta.hi));
    mapRange.onValueChange = [this]
    {
        if (! updating && onMap)
            onMap (macro, mapRange.getMinValue(), mapRange.getMaxValue());
    };
    mapRange.setTooltip ("The macro's range for " + juce::String (meta.label) + ": its value at macro 0 and at 1");
    addChildComponent (mapRange);
    configureKnob();
}

ArgKnob::~ArgKnob()
{
    knob.setLookAndFeel (nullptr);
    mapRange.setLookAndFeel (nullptr);
}

double ArgKnob::toNorm (double v) const
{
    if (meta.kind == ArgKind::Hz && meta.lo > 0.0)
        return std::log (juce::jlimit (meta.lo, meta.hi, v) / meta.lo) / std::log (meta.hi / meta.lo);
    return (juce::jlimit (meta.lo, meta.hi, v) - meta.lo) / (meta.hi - meta.lo);
}

juce::String ArgKnob::format (double v, bool offset) const
{
    if (offset)
    {
        if (meta.kind == ArgKind::Hz)
            return (v >= 0.0 ? "+" : "") + juce::String (v, 1) + " st";
        const double units = v * (meta.hi - meta.lo);
        if (meta.kind == ArgKind::Choice)
            return (units >= 0.0 ? "+" : "") + juce::String (units, 1);
        return (units >= 0.0 ? "+" : "") + juce::String (units, 2);
    }
    if (meta.kind == ArgKind::Hz)
        return hzText (v);
    if (meta.kind == ArgKind::Choice)
        return juce::String (juce::roundToInt (v));
    return juce::String (v, 2);
}

void ArgKnob::configureKnob()
{
    updating = true;
    const bool mapped = mode == ArgMode::Macro;
    const bool replace = mode == ArgMode::Replace || (mode == ArgMode::None && replaceSelected) || mapped;
    if (replace)
    {
        knob.setRange (meta.lo, meta.hi, meta.kind == ArgKind::Choice ? 1.0 : 0.0);
        if (meta.kind == ArgKind::Hz)
            knob.setSkewFactorFromMidPoint (std::sqrt (meta.lo * meta.hi));
        else
            knob.setSkewFactor (1.0);
        const double shown = mode == ArgMode::Replace ? value : (haveLive ? live : (meta.lo + meta.hi) * 0.5);
        knob.setValue (shown, juce::dontSendNotification);
        knob.setDoubleClickReturnValue (false, 0.0);
    }
    else
    {
        const double range = meta.kind == ArgKind::Hz ? kHzOffsetRange : 1.0;
        const double step = meta.kind == ArgKind::Hz ? 0.1 : (meta.kind == ArgKind::Choice ? 1.0 / (meta.hi - meta.lo) : 0.0);
        knob.setSkewFactor (1.0);
        knob.setRange (-range, range, step);
        knob.setValue (mode == ArgMode::Offset ? value : 0.0, juce::dontSendNotification);
        knob.setDoubleClickReturnValue (true, 0.0);
    }
    knob.getProperties().set ("bipolar", ! replace);
    knob.setEnabled (! mapped);
    mapRange.setVisible (mapped);
    if (mapped)
        mapRange.setMinAndMaxValues (juce::jmin (lo, hi), juce::jmax (lo, hi), juce::dontSendNotification);

    juce::String tip = juce::String (meta.label) + " (" + meta.name + ")";
    if (mapped)
        tip += "\nMapped to " + macroText + ". Right-click to change or remove the map.";
    else if (mode == ArgMode::Replace)
        tip += "\nReplaced by a fixed value. Click Offset to give it back to the code.";
    else
        tip += meta.kind == ArgKind::Hz
                   ? "\nOffset: semitones from the code's pitch (its modulation is kept). Double-click: no offset. "
                     "Right-click: map to a macro."
                   : "\nOffset: added to the code's value (its modulation is kept). Double-click: no offset. "
                     "Right-click: map to a macro.";
    setTooltip (tip);
    knob.setTooltip (tip);
    updating = false;
    repaint();
}

void ArgKnob::setState (ArgMode newMode, double newValue, int newMacro, double newLo, double newHi,
                        const juce::String& newMacroText)
{
    mode = newMode;
    value = newValue;
    macro = newMacro;
    lo = newLo;
    hi = newHi;
    macroText = newMacroText;
    if (mode == ArgMode::Replace)
        replaceSelected = true;
    else if (mode == ArgMode::Offset)
        replaceSelected = false;
    configureKnob();
}

void ArgKnob::setLive (double v)
{
    const bool changed = ! haveLive || std::abs (v - live) > 1.0e-6 * (1.0 + std::abs (v));
    live = v;
    haveLive = true;
    if (! changed)
        return;
    knob.getProperties().set ("live", toNorm (v));
    if (mode == ArgMode::Macro || (mode == ArgMode::None && replaceSelected))
    {
        updating = true;
        knob.setValue (v, juce::dontSendNotification);
        updating = false;
    }
    knob.repaint();
    repaint();
}

void ArgKnob::knobMoved()
{
    if (updating || ! onTweak)
        return;
    const double v = knob.getValue();
    if (replaceShown())
        onTweak (ArgTweak::kReplace, v);
    else if (std::abs (v) < 1.0e-9)
        onTweak (0, 0.0);
    else
        onTweak (ArgTweak::kOffset, v);
}

void ArgKnob::modeClicked (bool replace)
{
    if (! onTweak)
        return;
    if (replace)
    {
        replaceSelected = true;
        double v = haveLive ? live : (meta.lo + meta.hi) * 0.5;
        if (meta.kind == ArgKind::Choice)
            v = std::floor (v + 0.5);
        onTweak (ArgTweak::kReplace, juce::jlimit (meta.lo, meta.hi, v));
    }
    else
    {
        replaceSelected = false;
        onTweak (0, 0.0);
    }
    configureKnob();
}

void ArgKnob::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        showMenu();
        return;
    }
    if (e.eventComponent == this && mode != ArgMode::Macro && toggleArea.contains (e.getPosition()))
    {
        const bool replace = e.getPosition().x >= toggleArea.getCentreX();
        if (replace != replaceShown())
            modeClicked (replace);
    }
}

juce::PopupMenu ArgKnob::menu()
{
    juce::PopupMenu m;
    if (buildMenu)
        buildMenu (m);
    return m;
}

void ArgKnob::showMenu()
{
    auto m = menu();
    auto* top = getTopLevelComponent();
    // Inside the editor (one window in every host, and in its snapshots).
    if (auto* editor = findParentComponentOfClass<juce::AudioProcessorEditor>())
        top = editor;
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&knob).withParentComponent (top),
                     [safe = juce::Component::SafePointer<ArgKnob> (this)] (int result)
                     {
                         if (safe == nullptr || result == 0)
                             return;
                         auto& k = *safe;
                         if (result >= 1 && result <= 8)
                         {
                             double newLo = k.meta.lo, newHi = k.meta.hi;
                             if (k.mode == ArgMode::Macro)
                             {
                                 newLo = k.lo;
                                 newHi = k.hi;
                             }
                             else if (k.meta.kind == ArgKind::Hz)
                             {
                                 const double centre = k.haveLive && k.live > 0.0 ? k.live : 220.0;
                                 newLo = juce::jlimit (k.meta.lo, k.meta.hi, centre * 0.5);
                                 newHi = juce::jlimit (k.meta.lo, k.meta.hi, centre * 2.0);
                             }
                             if (k.onMap)
                                 k.onMap (result - 1, newLo, newHi);
                         }
                         else if (result == 20 && k.onUnmap)
                             k.onUnmap();
                         else if (result == 30 && k.onTweak)
                         {
                             if (k.mode == ArgMode::Macro && k.onUnmap)
                                 k.onUnmap();
                             k.replaceSelected = false;
                             k.onTweak (0, 0.0);
                         }
                     });
}

void ArgKnob::paint (juce::Graphics& g)
{
    using namespace Theme;
    auto r = getLocalBounds();
    g.setColour (text);
    g.setFont (font (smallSize, Weight::Medium));
    g.drawFittedText (meta.label, r.removeFromTop (16), juce::Justification::centred, 1, 0.85f);
    r.removeFromTop (kKnobSize + 2);
    juce::String setting;
    if (mode == ArgMode::Macro)
        setting = juce::String (macroText.upToFirstOccurrenceOf (" ", false, false)) + " " + format (lo, false)
                  + juce::String::fromUTF8 ("\xe2\x80\x93") + format (hi, false);
    else if (mode == ArgMode::Replace)
        setting = format (value, false);
    else if (mode == ArgMode::Offset)
        setting = format (value, true);
    else
        setting = "as coded";
    g.setColour (mode == ArgMode::None ? dimText : accent);
    g.setFont (font (captionSize, Weight::Medium));
    g.drawFittedText (setting, r.removeFromTop (13), juce::Justification::centred, 1, 0.8f);
    g.setColour (dimText);
    g.setFont (font (10.0f));
    g.drawFittedText (haveLive ? "live " + format (live, false) : juce::String ("live: play a note"),
                      r.removeFromTop (12), juce::Justification::centred, 1, 0.8f);

    // Offset | Replace, a small segmented toggle (hidden while mapped).
    if (mode != ArgMode::Macro && ! toggleArea.isEmpty())
    {
        const auto area = toggleArea.toFloat();
        const bool rep = replaceShown();
        const auto half = area.withWidth (area.getWidth() * 0.5f);
        const auto on = rep ? half.withX (area.getCentreX()) : half;
        g.setColour (control);
        g.fillRoundedRectangle (area, 4.0f);
        g.setColour (accent.withAlpha (0.16f));
        g.fillRoundedRectangle (on.reduced (1.0f), 3.0f);
        g.setColour (border.brighter (0.2f));
        g.drawRoundedRectangle (area.reduced (0.5f), 4.0f, hairline);
        g.fillRect (area.getCentreX() - 0.5f, area.getY() + 3.0f, hairline, area.getHeight() - 6.0f);
        g.setFont (font (10.5f, Weight::Medium));
        g.setColour (rep ? dimText : accent);
        g.drawText ("Offset", half.toNearestInt(), juce::Justification::centred);
        g.setColour (rep ? accent : dimText);
        g.drawText ("Replace", half.withX (area.getCentreX()).toNearestInt(), juce::Justification::centred);
    }
}

void ArgKnob::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (16);
    knob.setBounds (r.removeFromTop (kKnobSize).withSizeKeepingCentre (kKnobSize, kKnobSize));
    r.removeFromTop (2 + 13 + 12 + 3);
    auto row = r.removeFromTop (18).reduced (2, 0);
    mapRange.setBounds (row);
    toggleArea = row;
}

//==============================================================================
NodeInspector::NodeInspector (VibeSynthAudioProcessor& p) : processor (p)
{
    setButtonStyle (closeButton, ButtonStyle::Ghost);
    closeButton.setTooltip ("Close the inspector");
    closeButton.onClick = [this]
    {
        if (onClose)
            onClose();
    };
    addAndMakeVisible (closeButton);

    setButtonStyle (bypassButton, ButtonStyle::Segment);
    bypassButton.setClickingTogglesState (true);
    bypassButton.onClick = [this]
    {
        if (open)
            processor.setNodeBypass (id, bypassButton.getToggleState());
    };
    addAndMakeVisible (bypassButton);

    changeBox.setTextToShowWhenEmpty (juce::String::fromUTF8 ("Change this node\xe2\x80\xa6"),
                                      Theme::dimText);
    changeBox.setFont (Theme::font (Theme::smallSize));
    changeBox.onReturnKey = [this] { sendChange(); };
    addAndMakeVisible (changeBox);
    setButtonStyle (sendButton, ButtonStyle::Ghost);
    sendButton.onClick = [this] { sendChange(); };
    addAndMakeVisible (sendButton);
    setChangeEnabled (false);

    addAndMakeVisible (trace);
    setOpaque (true);
}

NodeInspector::~NodeInspector() = default;

void NodeInspector::setChangeEnabled (bool enabled)
{
    changeEnabled = enabled;
    changeBox.setEnabled (enabled);
    sendButton.setEnabled (enabled);
    const juce::String tip = enabled ? "Describe a change to this node; the model rewrites only what it needs"
                                     : "Coming in the next build";
    changeBox.setTooltip (tip);
    sendButton.setTooltip (tip);
}

void NodeInspector::sendChange()
{
    const auto text = changeBox.getText().trim();
    if (! changeEnabled || ! open || text.isEmpty() || ! onChangeRequested)
        return;
    onChangeRequested (juce::String (id), text);
}

void NodeInspector::setNode (const NodeRef& n)
{
    const bool same = open && n == current;
    current = n;
    open = true;
    if (! same)
    {
        entry = primaryLexiconEntry ((int) n.module);
        id = nodeId (n);
        rebuild();
    }
    refreshFromPatch();
    resized();
    repaint();
}

void NodeInspector::rebuild()
{
    knobs.clear();
    buffer.assign (NodeTrace::kPoints, 0.0f);
    trace.setData (buffer.data(), 0, false, false);
    if (entry == nullptr)
        return;
    for (int k = 0; k < entry->numArgs; ++k)
    {
        const LexiconArg& a = entry->args[k];
        if (! isEditableArg (a.kind))
            continue;
        auto knob = std::make_unique<ArgKnob> (a, k);
        const std::string name = a.name;
        knob->onTweak = [this, name] (int mode, double v) { processor.setNodeArg (id, name, mode, v); };
        knob->onMap = [this, name] (int macro, double lo, double hi)
        { processor.setNodeMacroMap (id, name, macro + 1, lo, hi); };
        knob->onUnmap = [this, name] { processor.clearNodeMacroMap (id, name); };
        auto* raw = knob.get();
        knob->buildMenu = [this, raw] (juce::PopupMenu& m)
        {
            const auto& patch = processor.getUiPatch();
            const unsigned read = macrosRead (patch);
            const auto* map = findMacroMap (patch, id, raw->arg().name);
            m.addSectionHeader ("Map " + juce::String (raw->arg().label) + " to a macro");
            for (int i = 0; i < 8; ++i)
            {
                const juce::String label = processor.getMacroLabel (i);
                const bool codeReads = ((read >> i) & 1u) != 0u;
                juce::String text = "Macro " + juce::String (i + 1);
                if (codeReads && label.isNotEmpty() && label != text)
                    text += juce::String::fromUTF8 (" \xc2\xb7 ") + label;
                if (! codeReads)
                    text += " (unused)";
                m.addItem (i + 1, text, true, map != nullptr && map->macro == i + 1);
            }
            if (map != nullptr)
                m.addItem (20, "Remove macro map");
            m.addSeparator();
            m.addItem (30, "Reset to the code's value", findArgTweak (patch, id, raw->arg().name) != nullptr || map != nullptr);
        };
        addAndMakeVisible (*knob);
        knobs.push_back (std::move (knob));
    }

    const bool canBypass = entry->bypass != BypassKind::None;
    bypassButton.setEnabled (canBypass);
    juce::String tip;
    switch (entry->bypass)
    {
        case BypassKind::PassArg: tip = "Bypass: pass the input straight through"; break;
        case BypassKind::Zero:
            tip = current.module == ModuleId::UtilSafeFb ? "Bypass: output 0 (feedback off)" : "Bypass: silence this source";
            break;
        case BypassKind::ForceArg: tip = "Bypass: mix 0 (both channels dry)"; break;
        case BypassKind::None:
            tip = current.module == ModuleId::EnvAdsr || current.module == ModuleId::EnvFollow
                      ? "Envelopes have no bypass"
                      : "This node has no bypass";
            break;
    }
    bypassButton.setTooltip (tip);
}

void NodeInspector::refreshFromPatch()
{
    if (! open || entry == nullptr)
        return;
    const auto& patch = processor.getUiPatch();
    const auto* t = findNodeTweak (patch, id);
    bypassButton.setToggleState (t != nullptr && t->bypass, juce::dontSendNotification);
    const unsigned read = macrosRead (patch);
    for (auto& k : knobs)
    {
        const char* name = k->arg().name;
        if (const auto* map = findMacroMap (patch, id, name))
        {
            const int i = map->macro - 1;
            const juce::String label = processor.getMacroLabel (i);
            juce::String text = "M" + juce::String (map->macro);
            const bool codeReads = i >= 0 && i < 8 && ((read >> i) & 1u) != 0u;
            text += codeReads ? " (" + label + ", which the patch code also reads; it keeps its label)"
                              : " (named " + juce::String (k->arg().label) + " on the macro panel)";
            k->setState (ArgMode::Macro, 0.0, i, map->min, map->max, text);
        }
        else if (const auto* a = findArgTweak (patch, id, name))
            k->setState (a->mode == ArgTweak::kReplace ? ArgMode::Replace : ArgMode::Offset, a->value, 0, 0, 1, {});
        else
            k->setState (ArgMode::None, 0.0, 0, 0, 1, {});
    }
    repaint();
}

void NodeInspector::tick()
{
    if (! open || ! isVisible())
        return;
    auto& bank = processor.nodeTraces();
    const TraceSink& sink = current.rack == RackKind::Master ? bank.master : bank.voice;
    const NodeTrace& t = sink.nodes[(int) current.module][current.slot];
    int n = 0;
    bool ring = false;
    buffer.resize (NodeTrace::kPoints);
    const bool have = readNodeTrace (t, buffer.data(), n, ring);
    // Live while the audio thread wrote it in the last ~0.3 s of blocks.
    const uint32_t age = sink.block - t.lastBlock.load (std::memory_order_relaxed);
    const bool live = have && age < 64u;
    if (isFilter (current.module) && entry != nullptr)
        trace.setFilter (current.module, (double) t.args[2].load(), (double) t.args[3].load(), sink.rate);
    else
        trace.setFilter (ModuleId::Count, 0, 0, sink.rate);
    trace.setData (buffer.data(), have ? n : 0, ring, live);
    if (have)
        for (auto& k : knobs)
            k->setLive ((double) t.args[k->index()].load (std::memory_order_relaxed));
}

void NodeInspector::showMapMenu (int knobIndex)
{
    if (knobIndex >= 0 && knobIndex < (int) knobs.size())
        knobs[(size_t) knobIndex]->showMenu();
}

void NodeInspector::paint (juce::Graphics& g)
{
    using namespace Theme;
    const auto bounds = getLocalBounds().toFloat();
    g.fillAll (background);
    g.setColour (panel);
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (border);
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius, hairline);
    if (! open)
        return;

    auto r = titleArea;
    g.setColour (text);
    g.setFont (mono (15.0f));
    g.drawFittedText (juce::String (id), r.removeFromTop (20), juce::Justification::centredLeft, 1, 0.8f);
    g.setColour (dimText);
    caps (g, juce::String (regionName (current.rack)) + juce::String::fromUTF8 (" \xc2\xb7 INSPECTOR"),
          r.removeFromTop (14), juce::Justification::centredLeft);
    if (entry != nullptr)
    {
        g.setFont (mono (10.5f));
        g.drawFittedText (juce::String (entry->name) + "(" + entry->params + ")", r.removeFromTop (14),
                          juce::Justification::centredLeft, 1, 0.75f);
    }

    // Hairlines between the three columns.
    g.setColour (border);
    g.fillRect ((float) traceArea.getX() - (float) grid - 0.5f, (float) grid * 1.5f, hairline,
                bounds.getHeight() - (float) grid * 3.0f);
    g.fillRect ((float) knobArea.getX() - (float) grid - 0.5f, (float) grid * 1.5f, hairline,
                bounds.getHeight() - (float) grid * 3.0f);
    if (knobs.empty())
    {
        g.setColour (dimText);
        g.setFont (font (smallSize));
        g.drawFittedText ("No settings: this node's inputs are wired by the code. Use Bypass, or describe a change.",
                          knobArea.reduced (grid, 0), juce::Justification::centred, 3, 0.9f);
    }
}

void NodeInspector::resized()
{
    using namespace Theme;
    auto r = getLocalBounds().reduced (grid * 2, grid + grid / 2);
    closeButton.setBounds (getWidth() - grid * 4 - 4, grid, grid * 3, grid * 3);
    r.removeFromRight (grid * 3);

    auto left = r.removeFromLeft (grid * 25);
    titleArea = left.removeFromTop (50);
    left.removeFromTop (grid / 2);
    bypassButton.setBounds (left.removeFromTop (24).withWidth (grid * 10));
    left.removeFromTop (grid);
    auto changeRow = left.removeFromTop (28);
    sendButton.setBounds (changeRow.removeFromRight (grid * 6));
    changeRow.removeFromRight (grid / 2);
    changeBox.setBounds (changeRow);
    r.removeFromLeft (grid * 2);

    traceArea = r.removeFromLeft (grid * 34);
    trace.setBounds (traceArea);
    r.removeFromLeft (grid * 2);
    knobArea = r;
    if (! knobs.empty())
    {
        const int w = juce::jmin (grid * 14, knobArea.getWidth() / (int) knobs.size());
        auto row = knobArea.withSizeKeepingCentre (w * (int) knobs.size(), knobArea.getHeight());
        for (auto& k : knobs)
            k->setBounds (row.removeFromLeft (w));
    }
}

} // namespace vibesynth::ui
