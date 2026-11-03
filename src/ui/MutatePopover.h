#pragma once

// M14b Mutate steering popover (SPEC §9.7).
// Opens anchored to the Mutate button with two sections:
// 1. Your direction: text field (placeholder, max 300 chars, session memory),
//    variations selector 1 2 3 4 5, [Mutate] button.
// 2. Auto: toggle chips Darker, Movement, Brighter, Weirder, Space,
//    [Auto-mutate] button.
// Remember chips and count in app settings.

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>
#include <vector>

#include "ai/Mutator.h"
#include "ui/Theme.h"

class VibeSynthAudioProcessor;

namespace vibesynth::ui {

class MutatePopover final : public juce::Component
{
public:
    explicit MutatePopover (VibeSynthAudioProcessor& processor);
    ~MutatePopover() override;

    void open (juce::Rectangle<int> anchorBounds, juce::Rectangle<int> parentBounds);
    void close();
    bool isOpen() const noexcept { return isVisible(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    std::function<void (const std::vector<MutateDirection>&)> onMutate;
    std::function<void()> onClose;

private:
    void updateButtons();
    void startSteeredMutate();
    void startAutoMutate();

    VibeSynthAudioProcessor& processor_;

    juce::TextButton closeButton;
    juce::TextEditor directionEditor;
    std::array<juce::TextButton, 5> countButtons;
    juce::TextButton mutateButton { "Mutate" };

    std::array<juce::TextButton, 5> chipButtons;
    juce::TextButton autoButton { "Auto-mutate" };

    int count_ = 3;
    std::array<bool, 5> chips_ { true, true, true, true, false };
    juce::String sessionText_;

    juce::Rectangle<int> headerArea;
    juce::Rectangle<int> steerHeaderArea;
    juce::Rectangle<int> autoHeaderArea;
    float dividerY_ = 0.0f;
};

} // namespace vibesynth::ui
