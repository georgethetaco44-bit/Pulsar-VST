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
// Pills are sized to their full labels (the font steps down only when a
// patch has too many wide names for the view).
//
// OutputMeter is the stereo meter in the bottom bar: a framed L/R pair of
// bars over dim tracks with tick marks at -36/-24/-12/-6/0 dBFS, a bright
// peak-hold mark per channel and amber when the peak is hot.

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "engine/NodeMeters.h"
#include "engine/NodeOverrides.h"
#include "patch/PatchTweaks.h"
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
    // M10: also redraws each pill's live trace (behind its label) when the
    // audio thread has written a new one.
    void tick (NodeMeterBank& bank, double nowMs, const TraceBank* traces = nullptr);

    // M10 (SPEC §9.6): clicking a pill opens the inspector. `popupMenu`:
    // a right-click (M11: the inspector opens on its Change box).
    std::function<void (const NodeRef&, bool popupMenu)> onNodeClicked;
    // The inspected node (outlined), or none.
    void setSelected (const NodeRef* node);
    // Which nodes carry tweaks (a dot) or are bypassed (dashed, dimmed).
    void setTweaks (const Patch& patch);
    bool hasNode (const NodeRef& node) const;
    juce::Rectangle<float> nodeBounds (const NodeRef& node) const;  // empty if absent
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

    void paint (juce::Graphics&) override;
    void resized() override;

    int numNodes() const noexcept { return (int) layout.nodes.size(); }
    // Snapshot tool: what is wrong with the current placement (a label wider
    // than its pill, pills overlapping, a pill outside the view); empty if
    // all is well. pillFont() is the label size in use.
    std::vector<std::string> placementProblems() const;
    float pillFont() const noexcept { return pillFontSize; }

private:
    void place();
    int nodeIndexAt (juce::Point<float> p) const;
    int indexOf (const NodeRef& node) const;
    NodeRef refOf (size_t i) const;

    static constexpr int kPillPoints = 48;
    struct PillTrace
    {
        std::array<float, kPillPoints> y {};  // -1..1 (bipolar) or 0..1 (unipolar), already scaled
        int n = 0;
        bool unipolar = false;
        uint32_t stamp = 0;     // TraceSink::block at the last read
    };

    NodeGraphLayout layout;
    std::vector<PillTrace> pillTraces;           // per layout node
    std::vector<uint8_t> tweakState;             // per layout node: 1 tweaked, 2 bypassed
    int selected = -1;
    std::string tweakKey;                         // the tweaks setTweaks() last saw
    bool known = false;
    std::vector<juce::Rectangle<float>> boxes;   // per layout node
    std::vector<float> levels;                   // per layout node, 0..1 after decay
    juce::Rectangle<int> graphArea, voiceArea, masterArea;
    float pillFontSize = 11.0f;                  // shrinks only if the full names don't fit
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
    float holds[2] = {};         // peak-hold position, 0..1
    double holdSetMs[2] = {};    // when the hold was last raised
    bool hot[2] = {};            // the held peak was at or above -0.5 dBFS
    double lastTickMs = 0.0;
};

} // namespace vibesynth::ui
