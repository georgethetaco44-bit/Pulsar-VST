// M9 header. See PromptPanel.h.

#include "ui/PromptPanel.h"

#include "ui/PulsarLookAndFeel.h"

namespace vibesynth::ui {

namespace {
const juce::String kDot = juce::String::fromUTF8 (" \xc2\xb7 ");
const juce::String kNoKey = "Add an API key in Settings to generate (or set VIBESYNTH_API_KEY).";
} // namespace

//==============================================================================
PromptEditor::PromptEditor()
{
    setMultiLine (true, true);
    setReturnKeyStartsNewLine (false);
    setScrollbarsShown (true);
    setFont (Theme::font (15.0f));
    setIndents (12, 10);
    setTitle ("Prompt");
}

bool PromptEditor::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::returnKey)
    {
        if (key.getModifiers().isShiftDown())
            insertTextAtCaret ("\n");
        else if (onSubmit)
            onSubmit();
        return true;
    }
    return TextEditor::keyPressed (key);
}

//==============================================================================
void StatusLine::setState (const HeaderState& s)
{
    if (s == state)
        return;
    state = s;
    setTooltip (s.detail.isNotEmpty() ? s.detail : s.text);
    setMouseCursor (isExpandable() ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void StatusLine::paint (juce::Graphics& g)
{
    using namespace Theme;
    auto r = getLocalBounds();
    const float cy = (float) r.getCentreY();
    juce::Colour dot = dimText, ink = dimText;
    juce::String line = state.text;
    switch (state.kind)
    {
        case HeaderState::Kind::Busy:
            dot = accent;
            ink = text;
            line = state.stage + kDot + juce::String (state.seconds) + " s";
            break;
        case HeaderState::Kind::Success: dot = accent; ink = text; break;
        case HeaderState::Kind::Error:   dot = warning; ink = warning; break;
        case HeaderState::Kind::Info:    break;
    }
    if (state.kind == HeaderState::Kind::Busy)
    {
        // Live: a soft halo around the dot.
        g.setColour (accent.withAlpha (0.18f));
        g.fillEllipse (2.0f, cy - 7.0f, 14.0f, 14.0f);
    }
    g.setColour (dot);
    g.fillEllipse (6.0f, cy - 3.0f, 6.0f, 6.0f);
    r.removeFromLeft (grid * 3);

    if (isExpandable())
    {
        auto more = r.removeFromRight (72);
        g.setColour (dimText);
        g.setFont (font (smallSize, Weight::Medium));
        g.drawText (expanded ? "Hide" : "Details", more, juce::Justification::centredRight);
        r.removeFromRight (grid);
    }
    g.setColour (ink);
    g.setFont (font (13.0f, state.kind == HeaderState::Kind::Info ? Weight::Regular : Weight::Medium));
    g.drawFittedText (line, r, juce::Justification::centredLeft, 1, 1.0f);
}

void StatusLine::mouseUp (const juce::MouseEvent& e)
{
    if (isExpandable() && e.mouseWasClicked() && onToggleDetail)
        onToggleDetail();
}

//==============================================================================
PromptPanel::PromptPanel()
{
    prompt.setTextToShowWhenEmpty ("Describe a sound, e.g. weeping neuro reese with a throat that closes on the mod wheel",
                                   Theme::dimText);
    prompt.setTooltip ("Describe a sound. Enter generates, Shift+Enter adds a line, Esc cancels.");
    prompt.onSubmit = [this] { actionClicked(); };
    prompt.onEscapeKey = [this]
    {
        if (state.generating && onCancel)
            onCancel();
    };
    addAndMakeVisible (prompt);

    setButtonStyle (actionButton, ButtonStyle::Primary);
    actionButton.setButtonText ("Generate");
    actionButton.onClick = [this] { actionClicked(); };
    addAndMakeVisible (actionButton);

    status.onToggleDetail = [this]
    {
        if (onToggleDetail)
            onToggleDetail();
    };
    addAndMakeVisible (status);
    setState (state);
}

void PromptPanel::actionClicked()
{
    if (state.generating)
    {
        if (onCancel)
            onCancel();
    }
    else if (state.canGenerate && onGenerate)
    {
        onGenerate (prompt.getText());
    }
}

void PromptPanel::setState (const HeaderState& s)
{
    state = s;
    if (s.generating)
    {
        setButtonStyle (actionButton, ButtonStyle::Live);
        actionButton.setButtonText ("Cancel");
        actionButton.setEnabled (true);
        actionButton.setTooltip ("Cancel the generation (Esc). The current patch keeps playing.");
    }
    else
    {
        setButtonStyle (actionButton, ButtonStyle::Primary);
        actionButton.setButtonText ("Generate");
        actionButton.setEnabled (s.canGenerate);
        actionButton.setTooltip (s.canGenerate ? "Write, test and load a patch for the prompt (Enter)" : kNoKey);
    }
    prompt.setTextToShowWhenEmpty (s.canGenerate ? "Describe a sound, e.g. weeping neuro reese with a throat that "
                                                   "closes on the mod wheel"
                                                 : kNoKey,
                                   Theme::dimText);
    prompt.repaint();
    status.setState (s);
}

void PromptPanel::paint (juce::Graphics& g)
{
    using namespace Theme;
    // Wordmark: a small pulse mark and the name, tracked out.
    const auto r = wordmarkArea.toFloat();
    const float cy = r.getY() + 32.0f;
    const float cx = r.getX() + 10.0f;
    g.setColour (dimText);
    g.drawEllipse (cx - 8.0f, cy - 8.0f, 16.0f, 16.0f, 1.5f);
    g.setColour (text);
    g.fillEllipse (cx - 3.0f, cy - 3.0f, 6.0f, 6.0f);
    g.setColour (text);
    g.setFont (font (19.0f, Weight::SemiBold).withExtraKerningFactor (0.22f));
    g.drawText ("PULSAR", wordmarkArea.withTrimmedLeft (grid * 4).withHeight (64), juce::Justification::centredLeft);
}

void PromptPanel::resized()
{
    using namespace Theme;
    auto r = getLocalBounds();
    wordmarkArea = r.removeFromLeft (grid * 18);
    r.removeFromLeft (grid);
    auto top = r.removeFromTop (grid * 8);
    actionButton.setBounds (top.removeFromRight (grid * 16));
    top.removeFromRight (grid * 2);
    prompt.setBounds (top);
    r.removeFromTop (grid);
    status.setBounds (r.withWidth (prompt.getWidth()).withHeight (grid * 3));
}

//==============================================================================
void ErrorDetail::setText (const juce::String& t)
{
    text = t;
    repaint();
}

namespace {
juce::TextLayout detailLayout (const juce::String& text, float width)
{
    juce::AttributedString s;
    s.append (text, Theme::font (13.0f), Theme::text);
    s.setLineSpacing (3.0f);
    juce::TextLayout layout;
    layout.createLayout (s, width);
    return layout;
}
} // namespace

int ErrorDetail::heightForWidth (int width) const
{
    const auto layout = detailLayout (text, (float) (width - Theme::grid * 5));
    return juce::jlimit (Theme::grid * 6, Theme::grid * 24, (int) std::ceil (layout.getHeight()) + Theme::grid * 4);
}

void ErrorDetail::paint (juce::Graphics& g)
{
    using namespace Theme;
    const auto r = getLocalBounds().toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (r, radius);
    g.setColour (warning.withAlpha (0.5f));
    g.drawRoundedRectangle (r.reduced (0.5f), radius, hairline);
    g.setColour (warning);
    g.fillRoundedRectangle (r.withWidth (3.0f).reduced (0.0f, 8.0f), 1.5f);
    auto inner = r.reduced ((float) grid * 2.0f, (float) grid * 2.0f).withTrimmedLeft ((float) grid);
    detailLayout (text, inner.getWidth()).draw (g, inner);
}

void ErrorDetail::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasClicked() && onClose)
        onClose();
}

} // namespace vibesynth::ui
