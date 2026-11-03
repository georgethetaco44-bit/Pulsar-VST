#pragma once

// M14b inline text editor overlay for typing values into knobs (SPEC §9.6, §9.7).
// Prefilled with the current display text and selected. Enter applies, Escape
// cancels, focus loss applies. Out-of-range values clamp. Text it can't parse
// flashes the field red and changes nothing.

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

#include "ui/KnobValueParser.h"
#include "ui/Theme.h"

namespace vibesynth::ui {

class ValueEditorOverlay final : public juce::Component, private juce::Timer
{
public:
    ValueEditorOverlay();
    ~ValueEditorOverlay() override;

    void open (juce::Rectangle<int> targetBounds,
               const juce::String& currentText,
               KnobUnitKind kind,
               double minVal, double maxVal,
               std::function<void (double newValue)> onApply);

    void open (juce::Rectangle<int> targetBounds,
               const juce::String& currentText,
               KnobUnitKind kind,
               double minVal, double maxVal,
               std::function<void (const KnobParseResult& res)> onApplyResult);

    void close();
    bool isOpen() const noexcept { return isVisible(); }

    void resized() override;
    void paint (juce::Graphics&) override;

    std::function<void()> onClose;

private:
    void submit (bool isFocusLost);
    void flashRed();
    void timerCallback() override;

    juce::TextEditor editor;
    KnobUnitKind kind_ = KnobUnitKind::Generic;
    double minVal_ = 0.0;
    double maxVal_ = 1.0;
    std::function<void (double)> onApply_;
    std::function<void (const KnobParseResult&)> onApplyResult_;
    bool flashing_ = false;
};

} // namespace vibesynth::ui
