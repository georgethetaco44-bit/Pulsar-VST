// M9 code viewer. See CodeView.h.

#include "ui/CodeView.h"

#include "engine/LexiconTable.h"
#include "patch/LexiconLint.h"
#include "ui/PulsarLookAndFeel.h"

namespace vibesynth::ui {

namespace {

bool identStart (juce::juce_wchar c) { return juce::CharacterFunctions::isLetter (c) || c == '_'; }
bool identBody (juce::juce_wchar c) { return juce::CharacterFunctions::isLetterOrDigit (c) || c == '_' || c == '.'; }

// SPEC §4.3: the variables Pulsar binds into the voice and master VMs, plus
// the one-sample feedback conventions.
const juce::StringArray& boundVariables()
{
    static const juce::StringArray vars { "freq", "note", "vel", "gate", "trig", "srate", "out", "mw",
                                          "fb1", "fb2", "prev", "bpm", "beat", "inl", "inr", "outl",
                                          "outr", "m1", "m2", "m3", "m4", "m5", "m6", "m7", "m8" };
    return vars;
}

bool isBuiltin (const juce::String& w)
{
    static const juce::StringArray keywords { "function", "local", "instance", "global", "globals", "this" };
    if (keywords.contains (w, true))
        return true;
    for (int i = 0; i < numAllowedBuiltins(); ++i)
        if (w.equalsIgnoreCase (allowedBuiltin (i).name))
            return true;
    return false;
}

} // namespace

EelTokeniser::Token EelTokeniser::classify (const juce::String& word)
{
    if (findLexiconEntry (word.toRawUTF8()) != nullptr)
        return lexicon;
    if (boundVariables().contains (word, true))
        return bound;
    if (isBuiltin (word))
        return builtin;
    return identifier;
}

int EelTokeniser::readNextToken (juce::CodeDocument::Iterator& src)
{
    src.skipWhitespace();
    const auto c = src.peekNextChar();
    if (c == 0)
        return identifier;

    if (c == '/')
    {
        src.skip();
        const auto n = src.peekNextChar();
        if (n == '/')
        {
            src.skipToEndOfLine();
            return comment;
        }
        if (n == '*')
        {
            src.skip();
            juce::juce_wchar last = 0;
            while (! src.isEOF())
            {
                const auto ch = src.nextChar();
                if (last == '*' && ch == '/')
                    break;
                last = ch;
            }
            return comment;
        }
        return op;
    }

    auto afterDot = src;
    afterDot.skip();
    if (juce::CharacterFunctions::isDigit (c) || c == '$'
        || (c == '.' && juce::CharacterFunctions::isDigit (afterDot.peekNextChar())))
    {
        // Numbers, hex, and $pi / $e / $phi / $x41 / $'a' constants.
        src.skip();
        if (c == '$' && src.peekNextChar() == '\'')
        {
            src.skip();
            while (! src.isEOF() && src.peekNextChar() != '\'' && src.peekNextChar() != '\n')
                src.skip();
            if (src.peekNextChar() == '\'')
                src.skip();
            return number;
        }
        while (identBody (src.peekNextChar()))
        {
            const auto ch = src.nextChar();
            if ((ch == 'e' || ch == 'E') && (src.peekNextChar() == '-' || src.peekNextChar() == '+') && c != '$')
                src.skip();
        }
        return number;
    }

    if (identStart (c))
    {
        juce::String word;
        while (identBody (src.peekNextChar()) && word.length() < 64)
            word += juce::String::charToString (src.nextChar());
        while (identBody (src.peekNextChar()))
            src.skip();
        return classify (word);
    }

    if (c == '"')
    {
        src.skip();
        while (! src.isEOF())
        {
            const auto ch = src.nextChar();
            if (ch == '\\')
                src.skip();
            else if (ch == '"' || ch == '\n')
                break;
        }
        return string;
    }

    src.skip();
    return op;
}

juce::CodeEditorComponent::ColourScheme EelTokeniser::getDefaultColourScheme()
{
    using namespace Theme;
    // Shades of the palette only: the accent marks the lexicon, a pale
    // accent tint the bound variables, everything else is text or dim.
    juce::CodeEditorComponent::ColourScheme cs;
    cs.set ("Error", warning);
    cs.set ("Comment", dimText.withAlpha (0.75f));
    cs.set ("Number", text);
    cs.set ("Lexicon", accent);
    cs.set ("Builtin", text);
    cs.set ("Bound", accent.interpolatedWith (text, 0.55f));
    cs.set ("Identifier", text.withAlpha (0.72f));
    cs.set ("Operator", dimText);
    cs.set ("String", text.withAlpha (0.72f));
    return cs;
}

//==============================================================================
void CodeView::Bar::resized()
{
    auto r = getLocalBounds();
    copy.setBounds (r.removeFromRight (Theme::grid * 9));
    r.removeFromRight (Theme::grid);
    const int w = Theme::grid * 9;
    auto seg = r.removeFromLeft (w * 3);
    for (auto& b : sections)
        b.setBounds (seg.removeFromLeft (w));
}

CodeView::CodeView()
{
    static const char* names[] = { "init", "voice", "master" };
    for (int i = 0; i < 3; ++i)
    {
        auto& b = bar.sections[(size_t) i];
        b.setButtonText (names[i]);
        setButtonStyle (b, ButtonStyle::Segment);
        b.setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < 2 ? juce::Button::ConnectedOnRight : 0));
        b.setTooltip (juce::String ("Show the ") + names[i] + " section");
        b.onClick = [this, i] { showSection (i); };
        bar.addAndMakeVisible (b);
    }
    setButtonStyle (bar.copy, ButtonStyle::Ghost);
    bar.copy.setTooltip ("Copy this section to the clipboard");
    bar.copy.onClick = [this] { juce::SystemClipboard::copyTextToClipboard (code[(size_t) section]); };
    bar.addAndMakeVisible (bar.copy);

    editor.setReadOnly (true);
    editor.setFont (Theme::mono (13.0f));
    editor.setLineNumbersShown (true);
    editor.setScrollbarThickness (8);
    editor.setTabSize (4, true);
    editor.setColourScheme (tokeniser.getDefaultColourScheme());
    editor.setTitle ("Patch code");
    addAndMakeVisible (editor);
    showSection (1);
}

void CodeView::setCode (const juce::String& init, const juce::String& voice, const juce::String& master)
{
    code = { init, voice, master };
    showSection (section);
}

void CodeView::showSection (int index)
{
    section = juce::jlimit (0, 2, index);
    for (int i = 0; i < 3; ++i)
        bar.sections[(size_t) i].setToggleState (i == section, juce::dontSendNotification);
    auto text = code[(size_t) section];
    if (text.trim().isEmpty())
        text = section == 2 ? "// (no master code: the voices pass straight to the output)"
                            : "// (empty)";
    document.replaceAllContent (text);
    document.clearUndoHistory();
    editor.scrollToLine (0);
}

juce::Array<juce::Component*> CodeView::focusOrder()
{
    return { &bar.sections[0], &bar.sections[1], &bar.sections[2], &bar.copy, &editor };
}

void CodeView::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (r, Theme::radius);
    g.setColour (Theme::border);
    g.drawRoundedRectangle (r.reduced (0.5f), Theme::radius, Theme::hairline);
}

void CodeView::resized()
{
    editor.setBounds (getLocalBounds().reduced (Theme::grid, Theme::grid / 2));
}

} // namespace vibesynth::ui
