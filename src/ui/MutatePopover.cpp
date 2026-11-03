// M14b Mutate steering popover. See MutatePopover.h.

#include "ui/MutatePopover.h"

#include "PluginProcessor.h"
#include "ui/PulsarLookAndFeel.h"

namespace vibesynth::ui {

namespace {
static const char* kChipLabels[5] = { "Darker", "Movement", "Brighter", "Weirder", "Space" };
static const char* kChipKeys[5] = { "darker", "movement", "brighter", "weirder", "space" };
} // namespace

MutatePopover::MutatePopover (VibeSynthAudioProcessor& processor)
    : processor_ (processor)
{
    using namespace Theme;

    setButtonStyle (closeButton, ButtonStyle::Ghost);
    closeButton.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
    closeButton.setTooltip ("Close");
    closeButton.onClick = [this] { close(); };
    addAndMakeVisible (closeButton);

    directionEditor.setTextToShowWhenEmpty ("e.g. more metallic, faster wobble, keep the sub clean", dimText);
    directionEditor.setInputRestrictions (300);
    directionEditor.setReturnKeyStartsNewLine (false);
    directionEditor.setFont (font (13.0f));
    directionEditor.setColour (juce::TextEditor::backgroundColourId, control);
    directionEditor.setColour (juce::TextEditor::outlineColourId, border);
    directionEditor.setColour (juce::TextEditor::focusedOutlineColourId, accent);
    directionEditor.setColour (juce::TextEditor::textColourId, text);
    directionEditor.onReturnKey = [this] { startSteeredMutate(); };
    directionEditor.onEscapeKey = [this] { close(); };
    directionEditor.onTextChange = [this]
    {
        sessionText_ = directionEditor.getText();
        updateButtons();
    };
    addAndMakeVisible (directionEditor);

    for (int i = 0; i < 5; ++i)
    {
        auto& b = countButtons[(size_t) i];
        b.setButtonText (juce::String (i + 1));
        setButtonStyle (b, ButtonStyle::Segment);
        b.setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0)
                             | (i < 4 ? juce::Button::ConnectedOnRight : 0));
        b.setClickingTogglesState (false);
        b.onClick = [this, i]
        {
            count_ = i + 1;
            auto& s = processor_.aiSettings();
            s.setMutateCount (count_);
            s.save();
            updateButtons();
        };
        addAndMakeVisible (b);
    }

    setButtonStyle (mutateButton, ButtonStyle::Primary);
    mutateButton.onClick = [this] { startSteeredMutate(); };
    addAndMakeVisible (mutateButton);

    for (int i = 0; i < 5; ++i)
    {
        auto& b = chipButtons[(size_t) i];
        b.setButtonText (kChipLabels[i]);
        setButtonStyle (b, ButtonStyle::Segment);
        b.setClickingTogglesState (false);
        b.onClick = [this, i]
        {
            chips_[(size_t) i] = ! chips_[(size_t) i];
            auto& s = processor_.aiSettings();
            s.setMutateChip (i, chips_[(size_t) i]);
            s.save();
            updateButtons();
        };
        addAndMakeVisible (b);
    }

    setButtonStyle (autoButton, ButtonStyle::Ghost);
    autoButton.onClick = [this] { startAutoMutate(); };
    addAndMakeVisible (autoButton);

    setAlwaysOnTop (true);
    setVisible (false);
}

MutatePopover::~MutatePopover() = default;

void MutatePopover::open (juce::Rectangle<int> anchorBounds, juce::Rectangle<int> parentBounds)
{
    // Load persisted settings
    const auto& s = processor_.aiSettings();
    count_ = s.mutateCount();
    for (int i = 0; i < 5; ++i)
        chips_[(size_t) i] = s.mutateChip (i);

    directionEditor.setText (sessionText_, false);
    updateButtons();

    constexpr int popW = 340;
    constexpr int popH = 210;

    int x = anchorBounds.getRight() - popW;
    x = juce::jlimit (parentBounds.getX() + Theme::grid,
                      parentBounds.getRight() - popW - Theme::grid, x);

    int y = anchorBounds.getBottom() + Theme::grid / 2;
    if (y + popH > parentBounds.getBottom() - Theme::grid)
        y = anchorBounds.getY() - popH - Theme::grid / 2;
    y = juce::jlimit (parentBounds.getY() + Theme::grid,
                      parentBounds.getBottom() - popH - Theme::grid, y);

    setBounds (x, y, popW, popH);
    setVisible (true);
    toFront (true);
    directionEditor.grabKeyboardFocus();
    repaint();
}

void MutatePopover::close()
{
    if (! isVisible())
        return;
    setVisible (false);
    if (onClose)
        onClose();
}

bool MutatePopover::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        close();
        return true;
    }
    return Component::keyPressed (key);
}

void MutatePopover::updateButtons()
{
    for (int i = 0; i < 5; ++i)
        countButtons[(size_t) i].setToggleState (i + 1 == count_, juce::dontSendNotification);

    mutateButton.setEnabled (directionEditor.getText().trim().isNotEmpty());

    int litCount = 0;
    for (int i = 0; i < 5; ++i)
    {
        chipButtons[(size_t) i].setToggleState (chips_[(size_t) i], juce::dontSendNotification);
        if (chips_[(size_t) i])
            ++litCount;
    }
    autoButton.setEnabled (litCount > 0);
}

void MutatePopover::startSteeredMutate()
{
    const auto text = directionEditor.getText().trim();
    if (text.isEmpty())
        return;
    sessionText_ = text;
    const auto dirs = Mutator::buildSteeredDirections (text.toStdString(), count_);
    close();
    if (onMutate)
        onMutate (dirs);
}

void MutatePopover::startAutoMutate()
{
    std::vector<std::string> keys;
    for (int i = 0; i < 5; ++i)
        if (chips_[(size_t) i])
            keys.push_back (kChipKeys[i]);

    if (keys.empty())
        return;

    const auto dirs = Mutator::buildAutoDirections (keys);
    close();
    if (onMutate)
        onMutate (dirs);
}

void MutatePopover::paint (juce::Graphics& g)
{
    using namespace Theme;
    const auto bounds = getLocalBounds().toFloat();

    g.setColour (panel.darker (0.15f));
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (accent.withAlpha (0.75f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.25f);

    g.setColour (accent);
    g.setFont (font (captionSize, Weight::SemiBold).withExtraKerningFactor (0.10f));
    g.drawText ("MUTATE", headerArea, juce::Justification::centredLeft);

    g.setColour (dimText);
    g.setFont (font (captionSize, Weight::SemiBold).withExtraKerningFactor (0.10f));
    g.drawText ("YOUR DIRECTION", steerHeaderArea, juce::Justification::centredLeft);
    g.drawText ("AUTO", autoHeaderArea, juce::Justification::centredLeft);

    if (dividerY_ > 0.0f)
    {
        g.setColour (border);
        g.drawLine (12.0f, dividerY_, (float) getWidth() - 12.0f, dividerY_, hairline);
    }
}

void MutatePopover::resized()
{
    using namespace Theme;
    auto r = getLocalBounds().reduced (12, 10);

    // Top title row
    auto top = r.removeFromTop (18);
    closeButton.setBounds (top.removeFromRight (20));
    headerArea = top;

    r.removeFromTop (6);

    // Section 1: Your direction
    steerHeaderArea = r.removeFromTop (14);
    r.removeFromTop (4);
    directionEditor.setBounds (r.removeFromTop (24));

    r.removeFromTop (6);
    auto countRow = r.removeFromTop (24);
    mutateButton.setBounds (countRow.removeFromRight (70));
    countRow.removeFromRight (8);
    const int countBtnW = 26;
    for (int i = 0; i < 5; ++i)
        countButtons[(size_t) i].setBounds (countRow.removeFromLeft (countBtnW));

    r.removeFromTop (8);
    dividerY_ = (float) r.getY();
    r.removeFromTop (8);

    // Section 2: Auto
    autoHeaderArea = r.removeFromTop (14);
    r.removeFromTop (4);
    auto chipsRow = r.removeFromTop (24);
    const int chipW = chipsRow.getWidth() / 5;
    for (int i = 0; i < 5; ++i)
        chipButtons[(size_t) i].setBounds (i < 4 ? chipsRow.removeFromLeft (chipW) : chipsRow);

    r.removeFromTop (6);
    autoButton.setBounds (r.removeFromTop (24).removeFromRight (100));
}

} // namespace vibesynth::ui
