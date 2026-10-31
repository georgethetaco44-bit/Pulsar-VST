#pragma once

// M9: the editor's LookAndFeel. Maps Theme onto JUCE's colour IDs and draws
// the controls flat: rounded panels, hairline borders, a cyan value arc on
// the rotary knobs and the accent only where something is active.
//
// Buttons pick a style with the "pulsarStyle" property (ButtonStyle):
//   primary  filled accent (Generate)
//   ghost    outline on the panel colour (the default)
//   tab      text only, accent underline when on
//   segment  one cell of a segmented control (Voice Mode, code sections)
//   live     outline with the accent glow (Cancel while generating)
//   danger   amber outline (confirming a delete: a warning)

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "ui/Theme.h"

namespace vibesynth::ui {

enum class ButtonStyle { Ghost, Primary, Tab, Segment, Live, Danger };
void setButtonStyle (juce::Button& b, ButtonStyle style);

// A square icon button drawn as a vector path (the settings gear).
class GearButton final : public juce::Button
{
public:
    GearButton() : juce::Button ("Settings") { setMouseClickGrabsKeyboardFocus (false); }
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
};

class PulsarLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    PulsarLookAndFeel();

    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

    // Buttons.
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted,
                               bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    // Knobs and the glide slider.
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float pos, float minPos,
                           float maxPos, juce::Slider::SliderStyle, juce::Slider&) override;

    // Text fields.
    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;

    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height,
                        bool vertical, int thumbStart, int thumbSize, bool over, bool down) override;
    int getDefaultScrollbarWidth() override { return 8; }

    juce::Rectangle<int> getTooltipBounds (const juce::String& text, juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawCornerResizer (juce::Graphics&, int w, int h, bool over, bool dragging) override;

private:
    juce::Typeface::Ptr regular;
};

} // namespace vibesynth::ui
