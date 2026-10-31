#pragma once

// M13 (SPEC §9.8): the MOD strip (2 LFO tiles + 2 ADSR envelope tiles with
// live moving playheads, drag-to-assign, and a settings popover on click),
// plus ModAwareSlider shared by MacroKnob and ArgKnob for modulation rings,
// ring-drag depth editing, drag-and-drop target, and live modulated dots.

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>

#include "engine/Modulators.h"
#include "patch/Patch.h"
#include "ui/Theme.h"

class VibeSynthAudioProcessor;

namespace vibesynth::ui {

// Distinct, theme-harmonious colours for the 4 modulators (never amber,
// which Theme.h reserves for warnings and errors).
inline juce::Colour modSourceColour (ModSource s) noexcept
{
    switch (s)
    {
        case ModSource::Lfo1: return juce::Colour (0xff3df5ff); // cyan (Theme::accent)
        case ModSource::Lfo2: return juce::Colour (0xff5ca8ff); // electric sky blue
        case ModSource::Env1: return juce::Colour (0xff3dffb5); // mint cyan-green
        case ModSource::Env2: return juce::Colour (0xffb87cff); // neon violet
    }
    return Theme::accent;
}

// A rotary juce::Slider that supports:
//   - right-click context menu callback
//   - dragging the outer modulation depth ring (or Shift-dragging) to change
//     bipolar modulation depth [-1..+1] without moving the underlying knob
//   - storing mod state in component properties ("modActive", "modSource",
//     "modDepth", "modLiveNorm") for LookAndFeel drawing.
class ModAwareSlider : public juce::Slider
{
public:
    ModAwareSlider();

    void setModulation (bool active, ModSource source, double depth, double liveNorm);
    bool hasModulation() const noexcept { return modActive_; }
    ModSource modSource() const noexcept { return modSource_; }
    double modDepth() const noexcept { return modDepth_; }

    std::function<void()> onRightClick;
    std::function<void (ModSource, double)> onModDepthChanged;

    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    bool isOnModRing (juce::Point<float> pos) const noexcept;
    void syncProps();

    bool modActive_ = false;
    ModSource modSource_ = ModSource::Lfo1;
    double modDepth_ = 0.0;
    double liveNorm_ = -1.0;
    bool draggingRing_ = false;
    double dragStartDepth_ = 0.0;
};

// Draws the coloured modulation depth arc + handle + live moving dot around
// a rotary knob. Called from PulsarLookAndFeel and InspectorKnobLook.
void drawKnobModulationRing (juce::Graphics& g, juce::Rectangle<float> bounds,
                             float pos, float startAngle, float endAngle,
                             juce::Slider& slider);

//==============================================================================
// Popover editor for one modulator (LFO 1, LFO 2, Env 1, or Env 2).
class ModPopover final : public juce::Component
{
public:
    explicit ModPopover (VibeSynthAudioProcessor& processor);
    ~ModPopover() override;

    void openFor (ModSource source);
    void close();
    bool isOpen() const noexcept { return isVisible(); }
    ModSource activeSource() const noexcept { return source_; }
    void refreshFromPatch();

    std::function<void()> onClose;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void applyLfoEdit (const std::function<void (ModLfoSpec&)>& fn);
    void applyEnvEdit (const std::function<void (ModEnvSpec&)>& fn);

    VibeSynthAudioProcessor& processor_;
    ModSource source_ = ModSource::Lfo1;
    bool updating_ = false;

    juce::TextButton closeButton_ { juce::String::fromUTF8 ("\xc3\x97") };

    // LFO controls
    std::array<juce::TextButton, static_cast<size_t> (LfoShape::Count)> shapeButtons_;
    juce::TextButton syncButton_ { "Sync" };
    juce::TextButton retrigButton_ { "Retrig" };
    juce::Slider rateSlider_;

    // Env controls (A, D, S, R)
    std::array<juce::Slider, 4> envKnobs_;

    juce::Rectangle<int> headerArea_, rateLabelArea_, envLabelAreas_[4];
};

//==============================================================================
// One compact modulator tile in the MOD strip.
class ModTile final : public juce::Component, public juce::SettableTooltipClient
{
public:
    ModTile (ModSource source, VibeSynthAudioProcessor& processor);

    void setSelected (bool selected);
    void setPlayhead (float phase, float value, bool voiceActive);
    void refreshFromPatch (const Patch& patch);

    std::function<void (ModSource)> onClicked;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    ModSource source_;
    VibeSynthAudioProcessor& processor_;
    ModLfoSpec lfoSpec_;
    ModEnvSpec envSpec_;
    int assignCount_ = 0;
    bool selected_ = false;
    bool draggingTile_ = false;
    float playhead_ = 0.0f;
    float liveVal_ = 0.0f;
    bool voiceActive_ = false;
};

//==============================================================================
// The always-visible MOD strip (4 tiles: LFO 1, LFO 2, ENV 1, ENV 2).
class ModStrip final : public juce::Component
{
public:
    explicit ModStrip (VibeSynthAudioProcessor& processor);

    void showPatch (const Patch& patch);
    void tick();
    void setSelectedSource (std::optional<ModSource> src);

    std::function<void (ModSource)> onTileClicked;

    void paint (juce::Graphics& g) override;
    void resized() override;

    ModTile& tile (int index) noexcept { return *tiles_[(size_t) index]; }

private:
    VibeSynthAudioProcessor& processor_;
    std::array<std::unique_ptr<ModTile>, kNumModSources> tiles_;
    juce::Rectangle<int> labelArea_;
};

} // namespace vibesynth::ui
