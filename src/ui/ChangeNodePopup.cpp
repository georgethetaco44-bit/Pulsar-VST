// M11 "Change this node" popup. See ChangeNodePopup.h.

#include "ui/ChangeNodePopup.h"

#include "ui/PulsarLookAndFeel.h"

namespace vibesynth::ui {

ChangeNodePopup::ChangeNodePopup()
{
    editor.setFont (Theme::font (14.0f));
    editor.setTextToShowWhenEmpty ("e.g. make it wobble at 1/4 notes, add grit before this", Theme::dimText);
    editor.setTitle ("Change instruction");
    editor.onSubmit = [this] { submit(); };
    editor.onEscapeKey = [this] { close(); };
    addAndMakeVisible (editor);

    setButtonStyle (changeButton, ButtonStyle::Primary);
    changeButton.onClick = [this] { submit(); };
    addAndMakeVisible (changeButton);
    setButtonStyle (cancelButton, ButtonStyle::Ghost);
    cancelButton.onClick = [this] { close(); };
    addAndMakeVisible (cancelButton);
    setWantsKeyboardFocus (true);
    setTitle ("Change this node");
}

void ChangeNodePopup::open (const juce::String& id, juce::Rectangle<int> anchor, bool canChangeNow)
{
    nodeId = id;
    canChange = canChangeNow;
    changeButton.setEnabled (canChange);
    changeButton.setTooltip (canChange ? "Ask the model for this change; the rest of the patch is kept"
                                       : "Needs an API key (Settings)");
    editor.clear();

    // Below the pill if it fits, else above; inside the parent.
    auto area = getParentComponent() != nullptr ? getParentComponent()->getLocalBounds() : anchor;
    juce::Rectangle<int> r (kWidth, kHeight);
    r.setCentre (anchor.getCentreX(), 0);
    r.setY (anchor.getBottom() + Theme::grid);
    if (r.getBottom() > area.getBottom() - Theme::grid)
        r.setY (anchor.getY() - Theme::grid - kHeight);
    setBounds (r.constrainedWithin (area.reduced (Theme::grid)));
    setVisible (true);
    toFront (true);
    editor.grabKeyboardFocus();
    repaint();
}

void ChangeNodePopup::close()
{
    if (! isVisible())
        return;
    setVisible (false);
    if (onClosed)
        onClosed();
}

void ChangeNodePopup::submit()
{
    const auto text = editor.getText().trim();
    if (text.isEmpty() || ! canChange || ! onSubmit)
        return;
    if (onSubmit (nodeId, text))
        close();
}

bool ChangeNodePopup::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        close();
        return true;
    }
    return false;
}

void ChangeNodePopup::resized()
{
    using namespace Theme;
    auto r = getLocalBounds().reduced (grid * 2);
    titleArea = r.removeFromTop (grid * 3);
    r.removeFromTop (grid);
    auto bottom = r.removeFromBottom (grid * 4);
    r.removeFromBottom (grid);
    editor.setBounds (r);
    changeButton.setBounds (bottom.removeFromRight (grid * 11));
    bottom.removeFromRight (grid);
    cancelButton.setBounds (bottom.removeFromRight (grid * 10));
    hintArea = bottom;
}

void ChangeNodePopup::paint (juce::Graphics& g)
{
    using namespace Theme;
    const auto b = getLocalBounds().toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (b, radius);
    g.setColour (accent.withAlpha (0.6f));
    g.drawRoundedRectangle (b.reduced (0.5f), radius, hairline);

    auto t = titleArea;
    g.setColour (accent);
    g.setFont (font (captionSize, Weight::SemiBold).withExtraKerningFactor (0.12f));
    const juce::String caps ("CHANGE");
    const int capsW = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), caps) + grid * 2;
    g.drawText (caps, t.removeFromLeft (capsW), juce::Justification::centredLeft);
    g.setColour (text);
    g.setFont (mono (14.0f));
    // "flt_lp:0" -> "flt_lp 0", "master/bus_sat#1" -> "master bus_sat #1" (the pill's own words).
    const auto label = nodeId.replace ("/", " ").replace (":", " ").replace ("#", " #");
    g.drawText (label, t, juce::Justification::centredLeft);

    g.setColour (dimText);
    g.setFont (font (smallSize));
    g.drawFittedText (canChange ? "The rest of the patch is kept." : "Needs an API key (Settings).", hintArea,
                      juce::Justification::centredLeft, 1, 1.0f);
}

} // namespace vibesynth::ui
