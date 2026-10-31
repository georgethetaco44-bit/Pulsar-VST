#pragma once

// M9 node visualizer (SPEC §9.6, read-only). Nodes are the touched
// (module, slot) pairs of the active patch, voice on the left and master
// on the right; edges are the patch's descriptive graph.edges between real
// nodes (layout: NodeLayout.h). The layout is computed only when the patch
// or its node list changes, never while the sound plays.
//
// tick() runs at 30 fps from the editor: it takes the peaks the audio
// thread published since the last frame (relaxed atomics, the max over
// sounding voices), keeps a ~150 ms visual decay, and repaints only while
// something glows. Glow radius and brightness follow the peak in dB.
//
// OutputMeter is the small stereo meter in the bottom bar, same scheme.

#include <juce_gui_basics/juce_gui_basics.h>

#include <string>
#include <vector>

#include "engine/NodeMeters.h"
#include "ui/NodeLayout.h"
#include "ui/Theme.h"

namespace vibesynth::ui {

// Peak (linear) -> 0..1 over -48..0 dBFS.
float meterLevel (float peak) noexcept;

class NodeView final : public juce::Component, public juce::SettableTooltipClient
{
public:
    NodeView();

    // `known` false: the node list is still being computed.
    void setGraph (const TouchedNodes& nodes, bool known, const std::string& graphJson);
    // Takes the meters and advances the decay; repaints only if needed.
    void tick (NodeMeterBank& bank, double nowMs);

    void paint (juce::Graphics&) override;
    void resized() override;

    int numNodes() const noexcept { return (int) layout.nodes.size(); }

private:
    void place();

    NodeGraphLayout layout;
    bool known = false;
    std::vector<juce::Rectangle<float>> boxes;   // per layout node
    std::vector<float> levels;                   // per layout node, 0..1 after decay
    juce::Rectangle<int> graphArea, voiceArea, masterArea;
    float dividerX = 0.0f;
    double lastTickMs = 0.0;
    bool glowing = false;
};

class OutputMeter final : public juce::Component, public juce::SettableTooltipClient
{
public:
    OutputMeter();
    void tick (float peakL, float peakR, double nowMs);
    void paint (juce::Graphics&) override;

private:
    float levels[2] = {};
    float peaks[2] = {};
    double lastTickMs = 0.0;
};

} // namespace vibesynth::ui
