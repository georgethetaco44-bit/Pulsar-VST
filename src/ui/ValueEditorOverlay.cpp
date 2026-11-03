// M14b inline text editor overlay. See ValueEditorOverlay.h.

#include "ui/ValueEditorOverlay.h"

namespace vibesynth::ui {

ValueEditorOverlay::ValueEditorOverlay()
{
    using namespace Theme;
    editor.setFont (font (12.0f, Weight::Medium));
    editor.setJustification (juce::Justification::centred);
    editor.setColour (juce::TextEditor::backgroundColourId, panel.darker (0.25f));
    editor.setColour (juce::TextEditor::outlineColourId, accent);
    editor.setColour (juce::TextEditor::focusedOutlineColourId, accent);
    editor.setColour (juce::TextEditor::textColourId, text);
    editor.setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.35f));

    editor.onReturnKey = [this] { submit (false); };
    editor.onEscapeKey = [this] { close(); };
    editor.onFocusLost = [this] { submit (true); };

    addAndMakeVisible (editor);
    setAlwaysOnTop (true);
    setVisible (false);
}

ValueEditorOverlay::~ValueEditorOverlay()
{
    stopTimer();
}

void ValueEditorOverlay::open (juce::Rectangle<int> targetBounds,
                               const juce::String& currentText,
                               KnobUnitKind kind,
                               double minVal, double maxVal,
                               std::function<void (double)> onApply)
{
    open (targetBounds, currentText, kind, minVal, maxVal,
          [apply = std::move (onApply)] (const KnobParseResult& res)
          {
              if (apply)
                  apply (res.value);
          });
}

void ValueEditorOverlay::open (juce::Rectangle<int> targetBounds,
                               const juce::String& currentText,
                               KnobUnitKind kind,
                               double minVal, double maxVal,
                               std::function<void (const KnobParseResult&)> onApplyResult)
{
    stopTimer();
    flashing_ = false;
    kind_ = kind;
    minVal_ = minVal;
    maxVal_ = maxVal;
    onApply_ = nullptr;
    onApplyResult_ = std::move (onApplyResult);

    // Reset colours
    using namespace Theme;
    editor.setColour (juce::TextEditor::backgroundColourId, panel.darker (0.25f));
    editor.setColour (juce::TextEditor::outlineColourId, accent);

    editor.setText (currentText.trim(), false);
    editor.selectAll();

    const int w = juce::jmax (52, targetBounds.getWidth() + 8);
    const int h = juce::jmax (18, targetBounds.getHeight() + 4);
    setBounds (targetBounds.withSizeKeepingCentre (w, h));

    setVisible (true);
    toFront (true);
    editor.grabKeyboardFocus();
    repaint();
}

void ValueEditorOverlay::close()
{
    stopTimer();
    flashing_ = false;
    setVisible (false);
    if (onClose)
        onClose();
}

void ValueEditorOverlay::submit (bool isFocusLost)
{
    if (! isVisible() || flashing_)
        return;

    const auto res = parseKnobValue (editor.getText(), kind_, minVal_, maxVal_);
    if (res.ok)
    {
        if (onApplyResult_)
            onApplyResult_ (res);
        else if (onApply_)
            onApply_ (res.value);
        close();
    }
    else
    {
        if (isFocusLost)
        {
            // Focus lost with invalid text: cancel and close
            close();
        }
        else
        {
            // Enter key with invalid text: flash red, keep open
            flashRed();
        }
    }
}

void ValueEditorOverlay::flashRed()
{
    flashing_ = true;
    editor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xcc8b0000));
    editor.setColour (juce::TextEditor::outlineColourId, Theme::warning);
    editor.repaint();
    startTimer (350);
}

void ValueEditorOverlay::timerCallback()
{
    stopTimer();
    flashing_ = false;
    using namespace Theme;
    editor.setColour (juce::TextEditor::backgroundColourId, panel.darker (0.25f));
    editor.setColour (juce::TextEditor::outlineColourId, accent);
    editor.repaint();
}

void ValueEditorOverlay::resized()
{
    editor.setBounds (getLocalBounds());
}

void ValueEditorOverlay::paint (juce::Graphics&) {}

} // namespace vibesynth::ui
