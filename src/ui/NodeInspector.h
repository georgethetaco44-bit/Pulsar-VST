#pragma once

// M10 node inspector (SPEC §9.6). Opens over the bottom page area when a
// node pill is clicked. Everything is knobs, clicks and menus; the code
// stays read-only.
//
//   left    the node id, its region and signature, Bypass, and the
//           "Change this node" box (SPEC §9.7; the editor wires
//           onChangeRequested to the processor and enables it with a key,
//           a loaded patch and nothing generating), with a hint line
//           under it: what happens, why it's off, or "Changing..."
//   centre  a large live trace (a triggered cycle, or a rolling ~2 s trace
//           for envelopes and slow chaos); flt_* nodes also draw their
//           magnitude response from the live cutoff/res (UI-side maths)
//   right   one knob per editable argument with its live value. Offset
//           (default; normalized amount, semitones for hz) or Replace
//           (a fixed value). Double-click clears an offset. Right-click:
//           "Map to macro N" (a min/max range slider appears), "Remove
//           macro map", "Reset".
//
// Edits go through the processor (setNodeArg & co.), which keeps the patch's
// tweak lists and the audio thread's override table in step. tick() runs at
// 30 fps from the editor and reads only the processor's TraceBank.

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

#include "engine/NodeOverrides.h"
#include "patch/PatchTweaks.h"
#include "ui/ModStrip.h"
#include "ui/Theme.h"

class VibeSynthAudioProcessor;

namespace vibesynth::ui {

// Knob drawing for the inspector: a bipolar arc (from the centre) in
// Offset mode, a live-value marker on the ring.
class InspectorKnobLook final : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float pos, float startAngle,
                           float endAngle, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float pos, float minPos,
                           float maxPos, juce::Slider::SliderStyle, juce::Slider&) override;
};

// The magnitude response (dB) of the inspector's filter at `hz`, from the
// live normalized cutoff and resonance, as Filters.h maps them. `module`
// is one of the flt_* ModuleIds; 0 dB otherwise.
double filterResponseDb (ModuleId module, double cutoff, double res, double hz, double voiceRate) noexcept;

class TraceDisplay final : public juce::Component
{
public:
    void setData (const float* points, int n, bool ring, bool live);
    void setFilter (ModuleId module, double cutoff, double res, double voiceRate);  // module Count: none
    void paint (juce::Graphics&) override;

private:
    std::vector<float> data;
    bool ring = false;
    bool live = false;
    ModuleId filter = ModuleId::Count;
    double cutoff = 0.5, res = 0.0, rate = 96000.0;
};

class ArgKnob final : public juce::Component,
                      public juce::SettableTooltipClient,
                      public juce::DragAndDropTarget
{
public:
    ArgKnob (const LexiconArg& meta, int argIndex);
    ~ArgKnob() override;

    // The saved state of this argument (from the patch).
    void setState (ArgMode mode, double value, int macro, double lo, double hi, const juce::String& macroText);
    void setModAssignment (const ModAssignment* mod);
    void setLive (double value);  // the effective value the function receives

    std::function<void (int mode, double value)> onTweak;   // mode 0 clears
    std::function<void (int macro, double lo, double hi)> onMap;
    std::function<void()> onUnmap;
    std::function<void (ModSource, double)> onAssignMod;
    std::function<void()> onRemoveMod;
    // Fills the right-click menu (the inspector knows the macro labels).
    std::function<void (juce::PopupMenu&)> buildMenu;

    // DragAndDropTarget (dropping a modulator tile onto an inspector knob).
    bool isInterestedInDragSource (const SourceDetails& dragSourceDetails) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails& dragSourceDetails) override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    juce::PopupMenu menu();          // what right-click shows
    void showMenu();                 // right-click (also the snapshot tool)
    const LexiconArg& arg() const noexcept { return meta; }
    int index() const noexcept { return argIndex; }
    double liveValue() const noexcept { return live; }

    ModAwareSlider knob;
    juce::Slider mapRange;   // two-value: the macro map's min and max

private:
    void configureKnob();
    void knobMoved();
    void modeClicked (bool replace);
    juce::String format (double v, bool offset) const;
    double toNorm (double v) const;  // a value in [lo, hi] to 0..1 on the knob's scale
    bool replaceShown() const noexcept
    {
        return mode == ArgMode::Replace || (mode == ArgMode::None && replaceSelected);
    }

    InspectorKnobLook look;
    LexiconArg meta;
    int argIndex;
    ArgMode mode = ArgMode::None;
    bool replaceSelected = false;  // the toggle shown while nothing is set
    double value = 0.0;
    int macro = 0;
    double lo = 0.0, hi = 1.0;
    double live = 0.0;
    bool haveLive = false;
    bool hasMod = false;
    ModSource modSource = ModSource::Lfo1;
    double modDepth = 0.0;
    bool dragHover = false;
    juce::String macroText;
    bool updating = false;
    juce::Rectangle<int> toggleArea;  // the Offset | Replace segments (drawn here)
};

class NodeInspector final : public juce::Component
{
public:
    explicit NodeInspector (VibeSynthAudioProcessor& processor);
    ~NodeInspector() override;

    void setNode (const NodeRef& node);
    bool hasNode() const noexcept { return open; }
    const NodeRef& node() const noexcept { return current; }
    // The patch's tweaks or macros changed (or a new patch): re-read them.
    void refreshFromPatch();
    void tick();

    std::function<void()> onClose;
    // "Change this node" (M11): Enter or Send calls this with the node id
    // and the instruction. The text stays while the change runs (and after
    // a refusal); clearChangeText() empties it once the change landed.
    std::function<void (const juce::String& nodeId, const juce::String& instruction)> onChangeRequested;
    // `whyNot` is the hint shown while disabled ("Needs an API key ...").
    void setChangeEnabled (bool enabled, const juce::String& whyNot = {});
    void setChangeRunning (bool running);
    void focusChangeBox();
    void setChangeText (const juce::String& t) { changeBox.setText (t, false); }
    void clearChangeText() { changeBox.clear(); }

    // Snapshot tool: opens a knob's right-click menu inside the editor.
    void showMapMenu (int knobIndex);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void rebuild();
    void sendChange();

    VibeSynthAudioProcessor& processor;
    NodeRef current;
    bool open = false;
    const LexiconEntry* entry = nullptr;
    std::string id;

    juce::TextButton closeButton { juce::String::fromUTF8 ("\xc3\x97") };
    juce::TextButton bypassButton { "Bypass" };
    juce::TextEditor changeBox;
    juce::TextButton sendButton { "Send" };
    TraceDisplay trace;
    std::vector<std::unique_ptr<ArgKnob>> knobs;
    juce::Rectangle<int> titleArea, traceArea, knobArea;
    bool changeEnabled = false;
    bool changeRunning = false;
    juce::String changeHint;
    juce::Rectangle<int> hintArea;
    std::vector<float> buffer;
};

} // namespace vibesynth::ui
