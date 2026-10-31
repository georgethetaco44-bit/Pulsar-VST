#pragma once

// M9 code viewer (SPEC §9.5): the active patch's init / voice / master
// code, read-only, in JetBrains Mono, with a light EEL2 tokeniser:
// comments, numbers, lexicon functions (accent; names come from
// engine/LexiconTable.h), EEL2 built-ins (the lint's allowed list), the
// bound variables of SPEC §4.3, and the rest. Copy puts the shown section
// on the clipboard.

#include <juce_gui_extra/juce_gui_extra.h>

#include <array>

#include "ui/Theme.h"

namespace vibesynth::ui {

class EelTokeniser final : public juce::CodeTokeniser
{
public:
    enum Token
    {
        error = 0,
        comment,
        number,
        lexicon,
        builtin,
        bound,
        identifier,
        op,
        string
    };

    int readNextToken (juce::CodeDocument::Iterator&) override;
    juce::CodeEditorComponent::ColourScheme getDefaultColourScheme() override;

    // Classification of one identifier (tests and the tokeniser).
    static Token classify (const juce::String& word);
};

class CodeView final : public juce::Component
{
public:
    CodeView();

    void setCode (const juce::String& init, const juce::String& voice, const juce::String& master);
    void showSection (int index);  // 0 init, 1 voice, 2 master
    int getSection() const noexcept { return section; }

    void resized() override;
    void paint (juce::Graphics&) override;

    // The tab bar shows this (section buttons + Copy).
    juce::Component& toolbar() { return bar; }
    juce::Array<juce::Component*> focusOrder();

private:
    struct Bar final : public juce::Component
    {
        void resized() override;
        std::array<juce::TextButton, 3> sections;
        juce::TextButton copy { "Copy" };
    };

    std::array<juce::String, 3> code;
    int section = 1;
    juce::CodeDocument document;
    EelTokeniser tokeniser;
    juce::CodeEditorComponent editor { document, &tokeniser };
    Bar bar;
};

} // namespace vibesynth::ui
