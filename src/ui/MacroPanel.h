#pragma once

// M9 macro panel (SPEC §9.2, §9.5): the 8 macros as rotary knobs in a 4 x 2
// grid with the patch's labels and values (the host's "Macro N" name is
// in each tooltip), then Voice Mode (Patch / Poly / Mono) and Glide.
// Attached to the APVTS, so host automation and the knobs stay in sync.

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <memory>

#include "patch/Patch.h"
#include "ui/Theme.h"

namespace vibesynth::ui {

class MacroKnob final : public juce::Component
{
public:
    MacroKnob (juce::AudioProcessorValueTreeState& state, int index);

    // The patch's label (empty = "Macro N") and default (double-click).
    // `used` false: the patch's code never reads this macro, so the knob is
    // dimmed, with no label or value ("Unused by this patch" in the tooltip).
    // M10: `drives` lists what macro maps this macro drives ("flt_lp:0
    // Cutoff"); a mapped macro the code doesn't read counts as used.
    void setPatchMacro (const juce::String& label, double defaultValue, bool used = true,
                        const juce::String& drives = {}, bool codeReads = true);

    void paint (juce::Graphics&) override;
    void resized() override;

    juce::Slider slider;

private:
    const int index;
    juce::String label;
    bool used = true;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

class VoiceModeControl final : public juce::Component
{
public:
    explicit VoiceModeControl (juce::RangedAudioParameter& param);

    void setPatchMode (const juce::String& mode);  // "poly" / "mono"
    int getMode() const noexcept { return mode; }
    void resized() override;

    std::array<juce::TextButton, 3> buttons;
    std::function<void()> onChange;

private:
    void update (int newMode);

    juce::ParameterAttachment attachment;
    int mode = 0;
};

class MacroPanel final : public juce::Component
{
public:
    explicit MacroPanel (juce::AudioProcessorValueTreeState& state);

    // A new patch: name, voice mode, the 8 labels + defaults and which
    // macros the code reads (bit i = m<i+1>, see macrosRead in LexiconLint.h).
    void setPatch (const juce::String& name, const juce::String& voiceMode,
                   const std::array<juce::String, 8>& labels, const std::array<double, 8>& defaults,
                   unsigned readMask);
    // M10 (SPEC §9.6): the same from the patch itself, with its macro maps.
    // A macro the code never reads but a map drives is un-dimmed and named
    // after the mapped argument; one the code reads keeps its label and its
    // tooltip says what else it drives.
    void showPatch (const vibesynth::Patch& patch);

    void paint (juce::Graphics&) override;
    void resized() override;

    // In keyboard focus order: the knobs, the voice mode, glide.
    juce::Array<juce::Component*> focusOrder();

private:
    void updateGlide();

    juce::AudioProcessorValueTreeState& state;
    std::array<std::unique_ptr<MacroKnob>, 8> knobs;
    VoiceModeControl voiceMode;
    juce::Slider glide;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> glideAttachment;
    juce::String patchName, patchMode;
    juce::Rectangle<int> titleArea, voiceLabelArea, glideLabelArea, glideValueArea;
};

} // namespace vibesynth::ui
