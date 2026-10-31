#pragma once

// M11 "Change this node" popup (SPEC §9.7). Right-click a node in the node
// view -> "Change this node..." opens it next to the node's pill:
//
//   CHANGE  flt_lp 0
//   [ make this filter wobble at 1/4 notes            ]
//   The rest of the patch is kept.        [Cancel] [Change]
//
// Enter sends (Shift+Enter adds a line), Esc closes. Temporary home of the
// text box until M10's inspector takes it (it calls the same
// VibeSynthAudioProcessor::requestNodeChange).

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "ui/PromptPanel.h"
#include "ui/Theme.h"

namespace vibesynth::ui {

class ChangeNodePopup final : public juce::Component
{
public:
    ChangeNodePopup();

    // Shows the popup for `nodeId` beside `anchor` (the pill, in the
    // parent's coordinates), inside the parent's bounds.
    void open (const juce::String& nodeId, juce::Rectangle<int> anchor, bool canChange);
    void close();
    void setText (const juce::String& t) { editor.setText (t, false); }
    const juce::String& getNodeId() const noexcept { return nodeId; }

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    // (node id, instruction); return true to close the popup.
    std::function<bool (const juce::String&, const juce::String&)> onSubmit;
    std::function<void()> onClosed;

    static constexpr int kWidth = Theme::grid * 46;
    static constexpr int kHeight = Theme::grid * 17;

private:
    void submit();

    juce::String nodeId;
    bool canChange = true;
    PromptEditor editor;
    juce::TextButton changeButton { "Change" };
    juce::TextButton cancelButton { "Cancel" };
    juce::Rectangle<int> titleArea, hintArea;
};

} // namespace vibesynth::ui
