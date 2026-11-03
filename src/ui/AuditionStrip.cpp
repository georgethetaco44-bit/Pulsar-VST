// M11 audition strip. See AuditionStrip.h.

#include "ui/AuditionStrip.h"

#include "patch/PatchLibrary.h"
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

    for (int i = 0; i < MutateView::kSlots; ++i)
    {
        auto& c = cardUis[(size_t) i];
        setButtonStyle (c.saveButton, ButtonStyle::Ghost);
        c.saveButton.setTooltip ("Save this variation to your patches");
        c.saveButton.onClick = [this, i] { startCardSave (i); };
        addChildComponent (c.saveButton);

        c.nameEditor.setFont (Theme::font (12.0f, Theme::Weight::Medium));
        c.nameEditor.setColour (juce::TextEditor::backgroundColourId, Theme::control.darker (0.2f));
        c.nameEditor.setColour (juce::TextEditor::outlineColourId, Theme::accent);
        c.nameEditor.setColour (juce::TextEditor::textColourId, Theme::text);
        c.nameEditor.onReturnKey = [this, i] { confirmCardSave (i); };
        c.nameEditor.onEscapeKey = [this, i] { cancelCardSave (i); };
        addChildComponent (c.nameEditor);

        setButtonStyle (c.confirmButton, ButtonStyle::Primary);
        c.confirmButton.onClick = [this, i] { confirmCardSave (i); };
        addChildComponent (c.confirmButton);

        setButtonStyle (c.cancelButton, ButtonStyle::Ghost);
        c.cancelButton.onClick = [this, i] { cancelCardSave (i); };
        addChildComponent (c.cancelButton);
    }

    setTitle ("Variations");
    setOpaque (true);
}

void AuditionStrip::setView (const MutateView& v)
{
    const bool isNewSession = ! juce::exactlyEqual (v.startedMs, view.startedMs);
    if (v == view && ! isNewSession)
        return;
    if (isNewSession)
    {
        for (auto& c : cardUis)
        {
            c.mode = CardMode::Normal;
            c.savedName.clear();
            c.pendingName.clear();
        }
    }
    view = v;
    updateButtons();
    updateCardComponents();
    resized();
    repaint();
}

void AuditionStrip::showCardSavedForSnapshot (int slot, const juce::String& name)
{
    if (slot >= 0 && slot < MutateView::kSlots)
    {
        cardUis[(size_t) slot].savedName = name;
        cardUis[(size_t) slot].mode = CardMode::Normal;
        updateCardComponents();
        repaint();
    }
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
    const bool ready = view.auditioning >= 0 && view.auditioning < view.activeSlots()
                    && view.slots[(size_t) view.auditioning].state == State::Ready;
    keepButton.setEnabled (ready);
    stopButton.setVisible (view.running);
}

void AuditionStrip::updateCardComponents()
{
    const int n = view.activeSlots();
    for (int i = 0; i < MutateView::kSlots; ++i)
    {
        auto& c = cardUis[(size_t) i];
        const bool active = i < n;
        const bool ready = active && view.slots[(size_t) i].state == State::Ready;
        if (! ready)
        {
            c.saveButton.setVisible (false);
            c.nameEditor.setVisible (false);
            c.confirmButton.setVisible (false);
            c.cancelButton.setVisible (false);
            continue;
        }
        c.saveButton.setVisible (c.mode == CardMode::Normal);
        c.nameEditor.setVisible (c.mode == CardMode::Naming);
        c.confirmButton.setVisible (c.mode != CardMode::Normal);
        c.cancelButton.setVisible (c.mode != CardMode::Normal);
    }
}

void AuditionStrip::startCardSave (int slot)
{
    if (slot < 0 || slot >= view.activeSlots())
        return;
    auto& c = cardUis[(size_t) slot];
    c.mode = CardMode::Naming;
    juce::String defaultName;
    if (c.savedName.isNotEmpty())
        defaultName = c.savedName;
    else if (view.takes)  // a take is a new sound: default to its own name, without " (take k)"
        defaultName = juce::String (view.slots[(size_t) slot].name).upToLastOccurrenceOf (" (take ", false, false);
    else
        defaultName = juce::String (view.baseName) + " " + juce::String (view.slots[(size_t) slot].direction);
    c.nameEditor.setText (defaultName, false);
    c.nameEditor.selectAll();
    c.confirmButton.setButtonText ("Save");
    c.confirmButton.setTooltip ("Save to your patches (Enter)");
    updateCardComponents();
    resized();
    c.nameEditor.grabKeyboardFocus();
    repaint();
}

void AuditionStrip::confirmCardSave (int slot)
{
    if (slot < 0 || slot >= view.activeSlots())
        return;
    auto& c = cardUis[(size_t) slot];
    if (c.mode == CardMode::Naming)
    {
        const auto name = c.nameEditor.getText().trim();
        if (name.isEmpty())
            return;
        const auto file = vibesynth::PatchLibrary::userPatchFile (vibesynth::PatchLibrary::userFolder(), name);
        if (file.exists())
        {
            c.pendingName = name;
            c.mode = CardMode::ConfirmReplace;
            c.confirmButton.setButtonText ("Replace");
            c.confirmButton.setTooltip ("Overwrite existing patch");
            updateCardComponents();
            resized();
            repaint();
            return;
        }
        juce::String error;
        if (onSaveVariation && onSaveVariation (slot, name, false, error))
        {
            c.savedName = name;
            c.mode = CardMode::Normal;
            updateCardComponents();
            resized();
            repaint();
        }
    }
    else if (c.mode == CardMode::ConfirmReplace)
    {
        const auto name = c.pendingName;
        juce::String error;
        if (onSaveVariation && onSaveVariation (slot, name, true, error))
        {
            c.savedName = name;
            c.mode = CardMode::Normal;
            updateCardComponents();
            resized();
            repaint();
        }
    }
}

void AuditionStrip::cancelCardSave (int slot)
{
    if (slot < 0 || slot >= MutateView::kSlots)
        return;
    auto& c = cardUis[(size_t) slot];
    c.mode = CardMode::Normal;
    updateCardComponents();
    resized();
    repaint();
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

    const int n = view.activeSlots();
    cards[0] = r.removeFromLeft (grid * 17);
    const int gap = grid;
    const int w = n > 0 ? (r.getWidth() - gap * n) / n : 0;
    for (int i = 0; i < n; ++i)
    {
        r.removeFromLeft (gap);
        cards[(size_t) i + 1] = (i == n - 1) ? r : r.removeFromLeft (w);
    }
    for (int i = n; i < MutateView::kSlots; ++i)
        cards[(size_t) i + 1] = {};

    for (int i = 0; i < MutateView::kSlots; ++i)
    {
        auto& c = cardUis[(size_t) i];
        const auto rCard = cards[(size_t) i + 1];
        if (rCard.isEmpty() || i >= n)
        {
            c.saveButton.setBounds ({});
            c.nameEditor.setBounds ({});
            c.confirmButton.setBounds ({});
            c.cancelButton.setBounds ({});
            continue;
        }
        auto inner = rCard.reduced (grid + grid / 2, grid);
        inner.removeFromTop (grid * 2 + grid / 2); // caption row (20 px)

        if (c.mode == CardMode::Naming)
        {
            c.nameEditor.setBounds (inner.removeFromTop (22));
            inner.removeFromTop (6);
            auto btnRow = inner.removeFromTop (20);
            const int btnW = (btnRow.getWidth() - 6) / 2;
            c.confirmButton.setBounds (btnRow.removeFromLeft (btnW));
            btnRow.removeFromLeft (6);
            c.cancelButton.setBounds (btnRow.removeFromLeft (btnW));
        }
        else if (c.mode == CardMode::ConfirmReplace)
        {
            inner.removeFromTop (22);
            inner.removeFromTop (6);
            auto btnRow = inner.removeFromTop (20);
            const int btnW = (btnRow.getWidth() - 6) / 2;
            c.confirmButton.setBounds (btnRow.removeFromLeft (btnW));
            btnRow.removeFromLeft (6);
            c.cancelButton.setBounds (btnRow.removeFromLeft (btnW));
        }
        else // CardMode::Normal
        {
            c.nameEditor.setBounds ({});
            c.confirmButton.setBounds ({});
            c.cancelButton.setBounds ({});
            inner.removeFromTop (grid * 3); // name row
            c.saveButton.setBounds (inner.removeFromBottom (20).removeFromLeft (56));
        }
    }
}

int AuditionStrip::cardAt (juce::Point<int> p) const
{
    if (cards[0].contains (p))
        return -1;
    const int n = view.activeSlots();
    for (int i = 0; i < n; ++i)
    {
        if (cards[(size_t) i + 1].contains (p))
        {
            if (cardUis[(size_t) i].mode != CardMode::Normal)
                return -2;
            return i;
        }
    }
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
    // The inspector's frame (they share the page area; one at a time).
    const auto bounds = getLocalBounds().toFloat();
    g.fillAll (background);
    g.setColour (panel);
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (border);
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius, hairline);

    auto title = titleRow;
    g.setColour (dimText);
    g.setFont (font (captionSize, Weight::SemiBold).withExtraKerningFactor (0.12f));
    juce::String caption = view.takes ? juce::String ("NEW TAKES")
                                      : "VARIATIONS OF " + juce::String (view.baseName).toUpperCase();
    g.drawText (caption, title.removeFromLeft (grid * 40), juce::Justification::centredLeft);

    const int n = view.activeSlots();
    paintCard (g, -1, cards[0]);
    for (int i = 0; i < n; ++i)
        paintCard (g, i, cards[(size_t) i + 1]);
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

    const auto* ui = (card >= 0 && card < MutateView::kSlots) ? &cardUis[(size_t) card] : nullptr;
    if (ui != nullptr && ui->mode == CardMode::ConfirmReplace)
    {
        g.setColour (warning);
        g.setFont (font (captionSize, Weight::SemiBold).withExtraKerningFactor (0.12f));
        g.drawText ("REPLACE FILE?", inner.removeFromTop (grid * 2), juce::Justification::centredLeft);
        inner.removeFromTop (grid / 2);

        g.setColour (text);
        g.setFont (font (12.0f, Weight::Medium));
        g.drawFittedText ("Overwrite existing?", inner.removeFromTop (grid * 3), juce::Justification::centredLeft, 2, 0.9f);
        return;
    }
    if (ui != nullptr && ui->mode == CardMode::Naming)
    {
        g.setColour (accent);
        g.setFont (font (captionSize, Weight::SemiBold).withExtraKerningFactor (0.12f));
        g.drawText ("SAVE AS", inner.removeFromTop (grid * 2), juce::Justification::centredLeft);
        return;
    }

    // Caption: the direction, or ORIGINAL.
    g.setColour (playing ? accent : dimText);
    g.setFont (font (captionSize, Weight::SemiBold).withExtraKerningFactor (0.12f));
    g.drawText (original ? juce::String ("ORIGINAL") : juce::String (s->direction).toUpperCase(),
                inner.removeFromTop (grid * 2), juce::Justification::centredLeft);
    inner.removeFromTop (grid / 2);

    // Name.
    juce::String name;
    if (original)
    {
        name = juce::String (view.baseName);
    }
    else if (ui != nullptr && ui->savedName.isNotEmpty())
    {
        name = "Saved: " + ui->savedName;
    }
    else if (s != nullptr && s->state == State::Ready)
    {
        name = juce::String (s->name);
    }

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
    if (ready && ! original)
        row.removeFromBottom (22); // space for Save button
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

//==============================================================================
HintBubble::HintBubble()
{
    setInterceptsMouseClicks (false, false);
}

void HintBubble::setText (const juce::String& t)
{
    text = t;
    repaint();
}

int HintBubble::widthForText() const
{
    return juce::GlyphArrangement::getStringWidthInt (Theme::font (Theme::smallSize, Theme::Weight::Medium), text)
         + Theme::grid * 4;
}

void HintBubble::paint (juce::Graphics& g)
{
    using namespace Theme;
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (panel);
    g.fillRoundedRectangle (b, b.getHeight() * 0.5f);
    g.setColour (accent);
    g.drawRoundedRectangle (b, b.getHeight() * 0.5f, 1.0f);
    g.setColour (Theme::text);
    g.setFont (font (smallSize, Weight::Medium));
    g.drawText (text, getLocalBounds(), juce::Justification::centred);
}

} // namespace vibesynth::ui
