// M9 LookAndFeel and the embedded fonts. See PulsarLookAndFeel.h.

#include "ui/PulsarLookAndFeel.h"

#include "PulsarFontData.h"
#include "ui/ModStrip.h"

namespace vibesynth::ui {

namespace {

// Loaded once while any editor is open (SharedResourcePointer), so
// reopening the editor doesn't re-parse the fonts each time it is cheap.
struct Typefaces
{
    juce::Typeface::Ptr regular, medium, semiBold, mono;

    Typefaces()
    {
        using namespace PulsarFontData;
        regular = juce::Typeface::createSystemTypefaceFor (InterRegular_ttf, (size_t) InterRegular_ttfSize);
        medium = juce::Typeface::createSystemTypefaceFor (InterMedium_ttf, (size_t) InterMedium_ttfSize);
        semiBold = juce::Typeface::createSystemTypefaceFor (InterSemiBold_ttf, (size_t) InterSemiBold_ttfSize);
        mono = juce::Typeface::createSystemTypefaceFor (JetBrainsMonoNLRegular_ttf,
                                                        (size_t) JetBrainsMonoNLRegular_ttfSize);
    }
};

juce::Font fontFor (juce::Typeface::Ptr tf, float height)
{
    return juce::Font (juce::FontOptions().withTypeface (std::move (tf)).withHeight (height));
}

ButtonStyle styleOf (const juce::Button& b)
{
    return static_cast<ButtonStyle> ((int) b.getProperties().getWithDefault ("pulsarStyle", 0));
}

} // namespace

juce::Font Theme::font (float height, Weight weight)
{
    juce::SharedResourcePointer<Typefaces> tf;
    switch (weight)
    {
        case Weight::Medium:   return fontFor (tf->medium, height);
        case Weight::SemiBold: return fontFor (tf->semiBold, height);
        case Weight::Regular:  break;
    }
    return fontFor (tf->regular, height);
}

juce::Font Theme::mono (float height)
{
    juce::SharedResourcePointer<Typefaces> tf;
    return fontFor (tf->mono, height);
}

void setButtonStyle (juce::Button& b, ButtonStyle style)
{
    b.getProperties().set ("pulsarStyle", (int) style);
    // Only keyboard traversal focuses a button, so clicks don't leave rings.
    b.setMouseClickGrabsKeyboardFocus (false);
    b.repaint();
}

void GearButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    using namespace Theme;
    const auto r = getLocalBounds().toFloat();
    g.setColour (down ? border : highlighted ? control : juce::Colours::transparentBlack);
    g.fillRoundedRectangle (r, radius);
    if (hasKeyboardFocus (false))
    {
        g.setColour (accent.withAlpha (0.8f));
        g.drawRoundedRectangle (r.reduced (1.0f), radius, 1.5f);
    }

    // Eight teeth around a ring with a hole.
    const auto c = r.getCentre();
    const float outer = juce::jmin (r.getWidth(), r.getHeight()) * 0.34f;
    const float inner = outer * 0.74f;
    juce::Path gear;
    constexpr int teeth = 8;
    for (int i = 0; i < teeth * 4; ++i)
    {
        const float a = juce::MathConstants<float>::twoPi * ((float) i - 0.5f) / (float) (teeth * 4);
        const float rad = (i % 4 < 2) ? outer : inner;
        const juce::Point<float> p (c.x + rad * std::sin (a), c.y - rad * std::cos (a));
        if (i == 0)
            gear.startNewSubPath (p);
        else
            gear.lineTo (p);
    }
    gear.closeSubPath();
    gear.addEllipse (c.x - outer * 0.36f, c.y - outer * 0.36f, outer * 0.72f, outer * 0.72f);
    gear.setUsingNonZeroWinding (false);
    g.setColour (highlighted || getToggleState() ? Theme::text : dimText);
    g.fillPath (gear);
}

//==============================================================================
PulsarLookAndFeel::PulsarLookAndFeel()
{
    juce::SharedResourcePointer<Typefaces> tf;
    regular = tf->regular;
    setDefaultSansSerifTypeface (regular);

    using namespace Theme;
    auto scheme = getDarkColourScheme();
    scheme.setUIColour (ColourScheme::windowBackground, background);
    scheme.setUIColour (ColourScheme::widgetBackground, control);
    scheme.setUIColour (ColourScheme::menuBackground, panel);
    scheme.setUIColour (ColourScheme::outline, border);
    scheme.setUIColour (ColourScheme::defaultText, text);
    scheme.setUIColour (ColourScheme::defaultFill, accent);
    scheme.setUIColour (ColourScheme::highlightedText, background);
    scheme.setUIColour (ColourScheme::highlightedFill, accent);
    scheme.setUIColour (ColourScheme::menuText, text);
    setColourScheme (scheme);

    setColour (juce::ResizableWindow::backgroundColourId, background);
    setColour (juce::DocumentWindow::backgroundColourId, background);
    setColour (juce::Label::textColourId, text);
    setColour (juce::TextEditor::backgroundColourId, control);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::outlineColourId, border);
    setColour (juce::TextEditor::focusedOutlineColourId, accent.withAlpha (0.7f));
    setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.25f));
    setColour (juce::TextEditor::highlightedTextColourId, text);
    setColour (juce::CaretComponent::caretColourId, accent);
    setColour (juce::TextButton::buttonColourId, control);
    setColour (juce::TextButton::buttonOnColourId, accent);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, background);
    setColour (juce::Slider::rotarySliderFillColourId, accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, border);
    setColour (juce::Slider::trackColourId, accent);
    setColour (juce::Slider::backgroundColourId, border);
    setColour (juce::Slider::thumbColourId, text);
    setColour (juce::ScrollBar::thumbColourId, border);
    setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::TooltipWindow::backgroundColourId, panel);
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, border);
    setColour (juce::PopupMenu::backgroundColourId, panel);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.18f));
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::highlightedTextColourId, text);
    setColour (juce::CodeEditorComponent::backgroundColourId, panel);
    setColour (juce::CodeEditorComponent::highlightColourId, accent.withAlpha (0.22f));
    setColour (juce::CodeEditorComponent::defaultTextColourId, text);
    setColour (juce::CodeEditorComponent::lineNumberBackgroundId, panel);
    setColour (juce::CodeEditorComponent::lineNumberTextId, dimText.withAlpha (0.55f));
}

juce::Typeface::Ptr PulsarLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    if (f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
        return regular;
    return LookAndFeel_V4::getTypefaceForFont (f);
}

//==============================================================================
void PulsarLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                                              bool highlighted, bool down)
{
    using namespace Theme;
    const auto style = styleOf (b);
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = b.getToggleState();
    const bool enabled = b.isEnabled();
    const bool focused = b.hasKeyboardFocus (false);

    switch (style)
    {
        case ButtonStyle::Primary:
        {
            auto fill = enabled ? accent : control;
            if (enabled && down)
                fill = fill.darker (0.15f);
            else if (enabled && highlighted)
                fill = fill.brighter (0.12f);
            g.setColour (fill);
            g.fillRoundedRectangle (r, radius);
            if (! enabled)
            {
                g.setColour (border);
                g.drawRoundedRectangle (r, radius, hairline);
            }
            break;
        }
        case ButtonStyle::Live:
        {
            // Static glow: the generating state is live.
            for (int i = 3; i >= 1; --i)
            {
                g.setColour (accent.withAlpha (0.05f * (float) (4 - i)));
                g.fillRoundedRectangle (r.expanded ((float) i * 2.0f), radius + (float) i * 2.0f);
            }
            g.setColour (highlighted ? control : panel);
            g.fillRoundedRectangle (r, radius);
            g.setColour (accent.withAlpha (down ? 1.0f : 0.85f));
            g.drawRoundedRectangle (r, radius, 1.5f);
            break;
        }
        case ButtonStyle::Tab:
        {
            if (on)
            {
                g.setColour (accent);
                g.fillRoundedRectangle (r.removeFromBottom (2.0f).reduced (6.0f, 0.0f), 1.0f);
            }
            break;
        }
        case ButtonStyle::Segment:
        {
            juce::Path p;
            const bool left = ! b.isConnectedOnLeft(), right = ! b.isConnectedOnRight();
            p.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), radius, radius,
                                   left, right, left, right);
            g.setColour (on ? accent.withAlpha (0.14f) : (highlighted ? border : control));
            g.fillPath (p);
            g.setColour (on ? accent.withAlpha (0.6f) : border);
            g.strokePath (p, juce::PathStrokeType (hairline));
            break;
        }
        case ButtonStyle::Danger:
        {
            g.setColour (down ? border : control);
            g.fillRoundedRectangle (r, radius);
            g.setColour (warning.withAlpha (highlighted ? 0.9f : 0.6f));
            g.drawRoundedRectangle (r, radius, hairline);
            break;
        }
        case ButtonStyle::Ghost:
        {
            g.setColour (down ? border : control);
            g.fillRoundedRectangle (r, radius);
            g.setColour (on ? accent.withAlpha (0.6f) : (highlighted && enabled ? dimText.withAlpha (0.6f) : border));
            g.drawRoundedRectangle (r, radius, hairline);
            break;
        }
    }

    if (focused)
    {
        g.setColour (accent.withAlpha (0.8f));
        g.drawRoundedRectangle (b.getLocalBounds().toFloat().reduced (1.0f), radius, 1.5f);
    }
}

juce::Font PulsarLookAndFeel::getTextButtonFont (juce::TextButton& b, int)
{
    const auto style = styleOf (b);
    if (style == ButtonStyle::Primary)
        return Theme::font (Theme::textSize, Theme::Weight::SemiBold);
    if (style == ButtonStyle::Tab)
        return Theme::font (13.0f, Theme::Weight::Medium);
    return Theme::font (13.0f, Theme::Weight::Medium);
}

void PulsarLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool highlighted, bool)
{
    using namespace Theme;
    const auto style = styleOf (b);
    const bool on = b.getToggleState();
    juce::Colour c = text;
    switch (style)
    {
        case ButtonStyle::Primary: c = b.isEnabled() ? background : dimText; break;
        case ButtonStyle::Live:    c = accent; break;
        case ButtonStyle::Tab:     c = on ? text : (highlighted ? text.withAlpha (0.85f) : dimText); break;
        case ButtonStyle::Segment: c = on ? accent : (highlighted ? text : dimText); break;
        case ButtonStyle::Ghost:   c = on ? accent : text; break;
        case ButtonStyle::Danger:  c = warning; break;
    }
    if (! b.isEnabled())
        c = c.withMultipliedAlpha (0.5f);
    g.setColour (c);
    g.setFont (getTextButtonFont (b, b.getHeight()));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (6, 0), juce::Justification::centred, 1, 0.9f);
}

//==============================================================================
void PulsarLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                                          float startAngle, float endAngle, juce::Slider& s)
{
    using namespace Theme;
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const float d = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto c = bounds.getCentre();
    const float arcR = d * 0.5f - 3.0f;
    const float faceR = arcR - 6.0f;
    const float angle = startAngle + pos * (endAngle - startAngle);
    const float alpha = s.isEnabled() ? 1.0f : 0.4f;
    const juce::PathStrokeType arcStroke (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    juce::Path track;
    track.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
    g.setColour (border.withMultipliedAlpha (alpha));
    g.strokePath (track, arcStroke);

    if (pos > 0.001f)
    {
        juce::Path value;
        value.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, startAngle, angle, true);
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
    g.drawLine (juce::Line<float> (c + dir * (faceR * 0.35f), c + dir * (faceR * 0.85f)), 2.0f);

    drawKnobModulationRing (g, bounds, pos, startAngle, endAngle, s);

    if (s.hasKeyboardFocus (false))
    {
        g.setColour (accent.withAlpha (0.8f));
        g.drawEllipse (c.x - arcR - 3.0f, c.y - arcR - 3.0f, (arcR + 3.0f) * 2.0f, (arcR + 3.0f) * 2.0f, 1.0f);
    }
}

void PulsarLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                                          float, float, juce::Slider::SliderStyle style, juce::Slider& s)
{
    if (style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, pos, 0.0f, 0.0f, style, s);
        return;
    }
    using namespace Theme;
    const float cy = (float) y + (float) height * 0.5f;
    const auto track = juce::Rectangle<float> ((float) x, cy - 2.0f, (float) width, 4.0f);
    g.setColour (border);
    g.fillRoundedRectangle (track, 2.0f);
    g.setColour (accent.withMultipliedAlpha (s.isEnabled() ? 1.0f : 0.4f));
    g.fillRoundedRectangle (track.withRight (pos), 2.0f);

    const float tr = s.isMouseOverOrDragging() ? 7.0f : 6.0f;
    g.setColour (text);
    g.fillEllipse (pos - tr, cy - tr, tr * 2.0f, tr * 2.0f);
    if (s.hasKeyboardFocus (false))
    {
        g.setColour (accent);
        g.drawEllipse (pos - tr - 2.0f, cy - tr - 2.0f, tr * 2.0f + 4.0f, tr * 2.0f + 4.0f, 1.0f);
    }
}

//==============================================================================
void PulsarLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& e)
{
    g.setColour (e.findColour (juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height), Theme::radius);
}

void PulsarLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& e)
{
    const bool focused = e.hasKeyboardFocus (true) && ! e.isReadOnly();
    g.setColour (e.findColour (focused ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId));
    g.drawRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f),
                            Theme::radius, Theme::hairline);
}

void PulsarLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height,
                                       bool vertical, int thumbStart, int thumbSize, bool over, bool down)
{
    if (thumbSize <= 0)
        return;
    auto thumb = vertical ? juce::Rectangle<int> (x, thumbStart, width, thumbSize)
                          : juce::Rectangle<int> (thumbStart, y, thumbSize, height);
    g.setColour (down ? Theme::dimText : over ? Theme::dimText.withAlpha (0.6f) : Theme::border);
    g.fillRoundedRectangle (thumb.toFloat().reduced (2.0f), 2.0f);
}

//==============================================================================
namespace {
juce::TextLayout tooltipLayout (const juce::String& text)
{
    juce::AttributedString s;
    s.setJustification (juce::Justification::topLeft);
    s.append (text, Theme::font (Theme::smallSize), Theme::text);
    juce::TextLayout layout;
    layout.createLayoutWithBalancedLineLengths (s, 320.0f);
    return layout;
}
} // namespace

juce::Rectangle<int> PulsarLookAndFeel::getTooltipBounds (const juce::String& text, juce::Point<int> screenPos,
                                                          juce::Rectangle<int> parentArea)
{
    const auto layout = tooltipLayout (text);
    const int w = (int) std::ceil (layout.getWidth()) + 16;
    const int h = (int) std::ceil (layout.getHeight()) + 12;
    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 16,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 8) : screenPos.y + 16,
                                 w, h)
        .constrainedWithin (parentArea);
}

void PulsarLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (Theme::border);
    g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, Theme::hairline);
    tooltipLayout (text).draw (g, r.reduced (8.0f, 6.0f));
}

void PulsarLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool over, bool dragging)
{
    // Two short hairlines in the corner, brighter while used.
    g.setColour ((over || dragging) ? Theme::dimText : Theme::border.brighter (0.2f));
    const float s = (float) juce::jmin (w, h);
    for (float f : { 0.45f, 0.75f })
        g.drawLine ((float) w - s * f, (float) h - 2.0f, (float) w - 2.0f, (float) h - s * f, 1.0f);
}

void PulsarLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (Theme::panel);
    g.setColour (Theme::border);
    g.drawRect (0, 0, width, height, 1);
}

} // namespace vibesynth::ui
