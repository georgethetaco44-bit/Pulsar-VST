// M11 audition strip. See AuditionStrip.h.

#include "ui/AuditionStrip.h"

#include "ui/PulsarLookAndFeel.h"

namespace vibesynth::ui {

namespace {
const juce::String kDot = juce::String::fromUTF8 (" \xc2\xb7 ");
using State = MutateSlotView::State;
} // namespace

AuditionStrip::AuditionStrip()
{
    setButtonStyle (keepButton, ButtonStyle::Primary);
    keepButton.setTooltip ("Use the playing variation (Undo goes back to the original)");
    keepButton.onClick = [this]
    {
        if (onKeep)
            onKeep();
    };
    setButtonStyle (discardButton, ButtonStyle::Ghost);
    discardButton.setTooltip ("Back to the original sound; closes the variations");
    discardButton.onClick = [this]
    {
        if (onDiscard)
            onDiscard();
    };
    setButtonStyle (stopButton, ButtonStyle::Ghost);
    stopButton.setTooltip ("Stop the variations still on their way (finished ones stay)");
    stopButton.onClick = [this]
    {
        if (onStop)
            onStop();
    };
    addAndMakeVisible (keepButton);
    addAndMakeVisible (discardButton);
    addChildComponent (stopButton);
    setTitle ("Variations");
}

void AuditionStrip::setView (const MutateView& v)
{
    if (v == view && juce::exactlyEqual (v.startedMs, view.startedMs))
        return;
    view = v;
    updateButtons();
    repaint();
}

void AuditionStrip::tick (double nowMs)
{
    if (! view.running)
        return;
    const int s = (int) ((nowMs - view.startedMs) / 1000.0);
    if (s != seconds)
    {
        seconds = s;
        repaint();
    }
}

void AuditionStrip::updateButtons()
{
    const bool ready = view.auditioning >= 0 && view.auditioning < MutateView::kSlots
                    && view.slots[(size_t) view.auditioning].state == State::Ready;
    keepButton.setEnabled (ready);
    stopButton.setVisible (view.running);
}

void AuditionStrip::resized()
{
    using namespace Theme;
    auto r = getLocalBounds().reduced (grid * 2, grid * 3 / 2);
    titleRow = r.removeFromTop (grid * 3);
    auto buttons = titleRow;
    keepButton.setBounds (buttons.removeFromRight (grid * 10));
    buttons.removeFromRight (grid);
    discardButton.setBounds (buttons.removeFromRight (grid * 11));
    buttons.removeFromRight (grid);
    stopButton.setBounds (buttons.removeFromRight (grid * 9));
    r.removeFromTop (grid);
    cards[0] = r.removeFromLeft (grid * 17);
    const int gap = grid;
    const int w = (r.getWidth() - gap * MutateView::kSlots) / MutateView::kSlots;
    for (int i = 0; i < MutateView::kSlots; ++i)
    {
        r.removeFromLeft (gap);
        cards[(size_t) i + 1] = r.removeFromLeft (w);
    }
}

int AuditionStrip::cardAt (juce::Point<int> p) const
{
    for (int i = 0; i <= MutateView::kSlots; ++i)
        if (cards[(size_t) i].contains (p))
            return i - 1;
    return -2;
}

void AuditionStrip::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mouseWasClicked())
        return;
    const int c = cardAt (e.getPosition());
    if (c == -2 || ! onAudition)
        return;
    if (c >= 0 && view.slots[(size_t) c].state != State::Ready)
        return;
    onAudition (c);
}

void AuditionStrip::mouseMove (const juce::MouseEvent& e)
{
    const int c = cardAt (e.getPosition());
    if (c != hover)
    {
        hover = c;
        const bool clickable = c == -1 || (c >= 0 && view.slots[(size_t) c].state == State::Ready);
        setMouseCursor (clickable ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void AuditionStrip::mouseExit (const juce::MouseEvent&)
{
    hover = -2;
    repaint();
}

juce::String AuditionStrip::getTooltip()
{
    const int c = cardAt (getMouseXYRelative());
    if (c == -1)
        return "The sound before Mutate: click to compare";
    if (c < 0)
        return {};
    const auto& s = view.slots[(size_t) c];
    switch (s.state)
    {
        case State::Ready:     return "Click to hear '" + juce::String (s.name) + "', then Keep to use it";
        case State::Failed:    return juce::String (s.detail.empty() ? s.text : s.detail);
        case State::Waiting:
        case State::Testing:   return "Being written and tested (" + juce::String (s.direction).toLowerCase() + ")";
        case State::Cancelled: return "Stopped before it finished";
        case State::Empty:     break;
    }
    return {};
}

void AuditionStrip::paint (juce::Graphics& g)
{
    using namespace Theme;
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (accent.withAlpha (0.35f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius, hairline);

    auto title = titleRow;
    g.setColour (dimText);
    g.setFont (font (captionSize, Weight::SemiBold).withExtraKerningFactor (0.12f));
    juce::String caption = "VARIATIONS OF " + juce::String (view.baseName).toUpperCase();
    g.drawText (caption, title.removeFromLeft (grid * 40), juce::Justification::centredLeft);

    for (int i = 0; i <= MutateView::kSlots; ++i)
        paintCard (g, i - 1, cards[(size_t) i]);
}

void AuditionStrip::paintCard (juce::Graphics& g, int card, juce::Rectangle<int> r) const
{
    using namespace Theme;
    const bool original = card < 0;
    const MutateSlotView* s = original ? nullptr : &view.slots[(size_t) card];
    const bool playing = view.auditioning == card;
    const bool ready = original || s->state == State::Ready;
    const bool failed = s != nullptr && s->state == State::Failed;
    const bool busy = s != nullptr && (s->state == State::Waiting || s->state == State::Testing);

    const auto rf = r.toFloat();
    g.setColour (ready && hover == card ? control.brighter (0.06f) : control);
    g.fillRoundedRectangle (rf, radius);
    if (playing)
    {
        g.setColour (accent.withAlpha (0.10f));
        g.fillRoundedRectangle (rf, radius);
        g.setColour (accent);
        g.drawRoundedRectangle (rf.reduced (0.75f), radius, 1.5f);
    }
    else
    {
        g.setColour (failed ? warning.withAlpha (0.35f) : border);
        g.drawRoundedRectangle (rf.reduced (0.5f), radius, hairline);
    }

    auto inner = r.reduced (grid + grid / 2, grid);
    // Caption: the direction, or ORIGINAL.
    g.setColour (playing ? accent : dimText);
    g.setFont (font (captionSize, Weight::SemiBold).withExtraKerningFactor (0.12f));
    g.drawText (original ? juce::String ("ORIGINAL") : juce::String (s->direction).toUpperCase(),
                inner.removeFromTop (grid * 2), juce::Justification::centredLeft);
    inner.removeFromTop (grid / 2);

    // Name.
    const juce::String name = original ? juce::String (view.baseName)
                                       : (s->state == State::Ready ? juce::String (s->name) : juce::String());
    g.setColour (text);
    g.setFont (font (15.0f, Weight::SemiBold));
    if (name.isNotEmpty())
        g.drawFittedText (name, inner.removeFromTop (grid * 3), juce::Justification::centredLeft, 1, 0.9f);
    else
        inner.removeFromTop (failed ? 0 : grid * 3);

    // Status line with a dot.
    juce::String line;
    juce::Colour dot = dimText, ink = dimText;
    if (original)
        line = playing ? "Playing" : "Click to compare";
    else
    {
        switch (s->state)
        {
            case State::Ready:
                line = playing ? "Playing" : "Ready" + kDot + "click to hear";
                dot = accent;
                break;
            case State::Waiting:
            case State::Testing:
                line = juce::String (s->text) + kDot + juce::String (seconds) + " s";
                dot = accent;
                ink = text;
                break;
            case State::Failed:
                line = juce::String (s->text);
                dot = ink = warning;
                break;
            case State::Cancelled: line = "Stopped"; break;
            case State::Empty:     break;
        }
    }
    if (playing)
        dot = ink = accent;
    auto row = inner;
    const int lines = failed ? 3 : 1;
    auto textArea = failed ? row : row.removeFromBottom (grid * 2);
    const float cy = (float) textArea.getY() + (failed ? 8.0f : (float) textArea.getHeight() * 0.5f);
    if (busy)
    {
        g.setColour (accent.withAlpha (0.18f));
        g.fillEllipse ((float) textArea.getX() - 3.0f, cy - 6.0f, 12.0f, 12.0f);
    }
    g.setColour (dot);
    g.fillEllipse ((float) textArea.getX() + 0.5f, cy - 2.5f, 5.0f, 5.0f);
    textArea.removeFromLeft (grid + grid / 2);
    g.setColour (ink);
    g.setFont (font (smallSize, failed || playing ? Weight::Medium : Weight::Regular));
    g.drawFittedText (line, textArea, failed ? juce::Justification::topLeft : juce::Justification::centredLeft,
                      lines, 1.0f);
}

} // namespace vibesynth::ui
