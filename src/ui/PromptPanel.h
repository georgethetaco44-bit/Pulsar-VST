#pragma once

// M9 header (SPEC §9.5): the PULSAR wordmark, the multi-line prompt box
// (Enter generates, Shift+Enter adds a line, Esc cancels), one button that
// is Generate or Cancel, and the status line: the generation stage with
// its elapsed seconds, the last result, or an amber error whose full
// reason is in the tooltip and opens below on click.

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "ui/Theme.h"

namespace vibesynth::ui {

// What the header shows; built from the processor (or faked by the
// snapshot tool).
struct HeaderState
{
    enum class Kind { Info, Busy, Success, Error };
    bool generating = false;
    bool canGenerate = false;    // an API key is set
    Kind kind = Kind::Info;
    juce::String text;           // the status line
    juce::String detail;         // full error text (tooltip + expanded view)
    juce::String stage;          // generating: "Waiting for model"
    int seconds = 0;             // generating: elapsed

    bool operator== (const HeaderState& o) const
    {
        return generating == o.generating && canGenerate == o.canGenerate && kind == o.kind && text == o.text
            && detail == o.detail && stage == o.stage && seconds == o.seconds;
    }
    bool operator!= (const HeaderState& o) const { return ! (*this == o); }
};

class PromptEditor final : public juce::TextEditor
{
public:
    PromptEditor();
    bool keyPressed (const juce::KeyPress&) override;

    std::function<void()> onSubmit;
};

class StatusLine final : public juce::Component, public juce::SettableTooltipClient
{
public:
    void setState (const HeaderState& s);
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool isExpandable() const { return state.kind == HeaderState::Kind::Error && state.detail.isNotEmpty(); }

    std::function<void()> onToggleDetail;
    bool expanded = false;

private:
    HeaderState state;
};

class PromptPanel final : public juce::Component
{
public:
    PromptPanel();

    void setState (const HeaderState& s);
    juce::String getPrompt() const { return prompt.getText(); }
    void setPrompt (const juce::String& t) { prompt.setText (t, false); }

    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void (const juce::String&)> onGenerate;
    std::function<void()> onCancel;
    std::function<void()> onToggleDetail;

    PromptEditor prompt;
    juce::TextButton actionButton;
    StatusLine status;

private:
    void actionClicked();

    HeaderState state;
    juce::Rectangle<int> wordmarkArea;
};

// The expanded error reason, placed under the status line by the editor.
class ErrorDetail final : public juce::Component
{
public:
    void setText (const juce::String& t);
    int heightForWidth (int width) const;
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

    std::function<void()> onClose;

private:
    juce::String text;
};

} // namespace vibesynth::ui
