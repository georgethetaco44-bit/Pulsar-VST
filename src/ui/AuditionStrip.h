#pragma once

// M11 audition strip (SPEC §9.7). While a Mutate session is open it covers
// the bottom page (Presets / Code / History) in the node inspector's place
// and frame (one of the two at a time): the original and the 4 variations
// as cards, filled in as each one finishes.
//
//   [ORIGINAL]  [DARKER]  [MOVEMENT]  [BRIGHTER]  [WEIRDER]
//    Reese       Dark Reese  Waiting · 9 s  Failed: ...  Testing
//
// Clicking a card swaps it in (the original too, to compare); the playing
// card has the accent outline. Keep makes the playing variation the active
// patch, Discard goes back to the original and closes the strip, Stop
// cancels what is still in flight (finished cards stay). A failed card
// shows the mapped reason in amber; the full reason is its tooltip.

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "ai/MutateView.h"
#include "ui/Theme.h"

namespace vibesynth::ui {

class AuditionStrip final : public juce::Component, public juce::TooltipClient
{
public:
    AuditionStrip();

    void setView (const MutateView& v);
    // Elapsed seconds on waiting cards (call at the editor's frame rate;
    // repaints only when the second changes).
    void tick (double nowMs);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    juce::String getTooltip() override;

    std::function<void (int slot)> onAudition;  // -1 = the original
    std::function<void()> onKeep;
    std::function<void()> onDiscard;
    std::function<void()> onStop;

    juce::TextButton keepButton { "Keep" };
    juce::TextButton discardButton { "Discard" };
    juce::TextButton stopButton { "Stop" };

private:
    int cardAt (juce::Point<int> p) const;  // -1 original, 0..3 slots, -2 none
    void paintCard (juce::Graphics& g, int card, juce::Rectangle<int> r) const;
    void updateButtons();

    MutateView view;
    std::array<juce::Rectangle<int>, MutateView::kSlots + 1> cards;  // [0] = original
    juce::Rectangle<int> titleRow;
    int hover = -2;
    int seconds = 0;
};

// A short hint over the node view while the strip is open ("Keep or
// Discard the variation first"); the editor places it and hides it again.
class HintBubble final : public juce::Component
{
public:
    HintBubble();
    void setText (const juce::String& t);
    int widthForText() const;
    void paint (juce::Graphics&) override;

private:
    juce::String text;
};

} // namespace vibesynth::ui
