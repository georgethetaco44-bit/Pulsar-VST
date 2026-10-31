// Model reply -> Patch (SPEC §6.2). See ResponseParser.h.

#include "ai/ResponseParser.h"

#include <juce_core/juce_core.h>

namespace vibesynth {

namespace {

constexpr int kMaxNameChars = 48;
constexpr int kMaxLabelChars = 24;

struct Block
{
    juce::String info;   // lower-case text after the opening ```
    juce::String body;
};

// Fenced blocks in order. An unclosed block runs to the end of the reply.
std::vector<Block> fencedBlocks (const juce::String& reply)
{
    std::vector<Block> blocks;
    juce::StringArray lines;
    lines.addLines (reply);
    bool inBlock = false;
    Block current;
    for (const auto& raw : lines)
    {
        const auto line = raw.trim();
        if (! line.startsWith ("```"))
        {
            if (inBlock)
                current.body << raw << "\n";
            continue;
        }
        if (inBlock)
        {
            blocks.push_back (current);
            inBlock = false;
            continue;
        }
        current = {};
        current.info = line.substring (3).trim().toLowerCase();
        inBlock = true;
    }
    if (inBlock)
        blocks.push_back (current);
    return blocks;
}

juce::StringArray infoTokens (const juce::String& info)
{
    juce::StringArray t;
    t.addTokens (info.replaceCharacters (":,{}", "    "), " \t", "");
    t.removeEmptyStrings();
    for (auto& s : t)
        s = s.trimCharactersAtStart ("@.");
    return t;
}

bool isEelLang (const juce::String& t)
{
    return t == "eel" || t == "eel2" || t == "jsfx";
}

bool isSection (const juce::String& t)
{
    return t == "init" || t == "voice" || t == "master";
}

// "voice" from "eel voice", "eel2 voice", "voice"; empty if the block isn't
// a code section. An "eel" block without a name takes it from a first-line
// comment such as "// voice".
juce::String sectionOf (const Block& b, bool& unnamedEel)
{
    unnamedEel = false;
    const auto t = infoTokens (b.info);
    for (const auto& s : t)
        if (isSection (s))
            return s;
    if (t.isEmpty() || ! isEelLang (t[0]))
        return {};
    juce::StringArray lines;
    lines.addLines (b.body);
    for (const auto& l : lines)
    {
        const auto line = l.trim();
        if (line.isEmpty())
            continue;
        if (line.startsWith ("//"))
        {
            const auto words = infoTokens (line.substring (2).toLowerCase());
            if (words.size() >= 1 && isSection (words[0]))
                return words[0];
        }
        break;
    }
    unnamedEel = true;
    return {};
}

// Trailing commas before } or ] (outside strings), a common slip that
// juce::JSON rejects.
juce::String withoutTrailingCommas (const juce::String& s)
{
    juce::String out;
    out.preallocateBytes (s.getNumBytesAsUTF8());
    bool inString = false;
    const int n = s.length();
    for (int i = 0; i < n; ++i)
    {
        const auto c = s[i];
        if (inString)
        {
            out << juce::String::charToString (c);
            if (c == '\\' && i + 1 < n)
                out << juce::String::charToString (s[++i]);
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"')
            inString = true;
        if (c == ',')
        {
            int j = i + 1;
            while (j < n && juce::CharacterFunctions::isWhitespace (s[j]))
                ++j;
            if (j < n && (s[j] == '}' || s[j] == ']'))
                continue;
        }
        out << juce::String::charToString (c);
    }
    return out;
}

bool isNumber (const juce::var& v)
{
    return v.isInt() || v.isInt64() || v.isDouble();
}

std::string fmt (double v)
{
    return juce::String (v, 3).toStdString();
}

ResponseParseResult fail (PatchStatus s, std::string error)
{
    ResponseParseResult r;
    r.status = s;
    r.error = std::move (error);
    return r;
}

bool hasCodeString (const juce::var& root)
{
    for (const char* key : { "code", "init", "voice", "master" })
    {
        const juce::var v = root.getProperty (key, juce::var());
        if (v.isString() && v.toString().trim().isNotEmpty())
            return true;
        if (v.isObject())
            return true;
    }
    return false;
}

} // namespace

ResponseParseResult parseResponse (const std::string& replyText)
{
    if (replyText.size() > Patch::kMaxFileBytes)
        return fail (PatchStatus::TooLarge, "reply too large (max 256 KB)");
    const std::string text = normaliseLineEndings (replyText);
    const auto reply = juce::String::fromUTF8 (text.data(), (int) text.size());
    const auto blocks = fencedBlocks (reply);

    const Block* json = nullptr;
    const Block* sections[3] = {};   // init, voice, master
    const Block* unnamed = nullptr;
    int numUnnamed = 0;
    for (const auto& b : blocks)
    {
        const auto t = infoTokens (b.info);
        if (json == nullptr && t.size() >= 1 && (t[0] == "json" || t[0] == "jsonc"))
        {
            json = &b;
            continue;
        }
        bool unnamedEel = false;
        const auto s = sectionOf (b, unnamedEel);
        const int idx = s == "init" ? 0 : s == "voice" ? 1 : s == "master" ? 2 : -1;
        if (idx >= 0 && sections[idx] == nullptr)
            sections[idx] = &b;
        if (unnamedEel)
        {
            unnamed = &b;
            ++numUnnamed;
        }
    }

    if (json == nullptr)
        return fail (PatchStatus::BadJson, "no ```json header block: start the reply with ```json {...} ```, "
                                           "then ```eel init, ```eel voice and ```eel master blocks");
    juce::var root;
    const auto parsed = juce::JSON::parse (withoutTrailingCommas (json->body), root);
    if (parsed.failed() || ! root.isObject())
        return fail (PatchStatus::BadJson, "invalid JSON in the ```json block: "
                                               + (parsed.failed() ? parsed.getErrorMessage().toStdString()
                                                                  : std::string ("expected an object")));

    ResponseParseResult r;
    Patch& p = r.patch;

    // Code belongs in ```eel blocks (escaping it in JSON strings breaks).
    if (sections[1] == nullptr && numUnnamed == 1)
    {
        sections[1] = unnamed;
        r.notes.push_back ("took the unnamed ```eel block as voice");
    }
    if (hasCodeString (root))
    {
        if (sections[1] == nullptr)
            return fail (PatchStatus::BadField, "code inside JSON strings: put the code in ```eel init, "
                                                "```eel voice and ```eel master blocks, not in the JSON");
        r.notes.push_back ("ignored code inside the JSON (used the ```eel blocks)");
    }
    if (sections[1] == nullptr)
        return fail (PatchStatus::MissingVoice, "no ```eel voice block");

    std::string* const targets[3] = { &p.init, &p.voice, &p.master };
    const char* const names[3] = { "init", "voice", "master" };
    for (int i = 0; i < 3; ++i)
    {
        if (sections[i] == nullptr)
            continue;
        *targets[i] = sections[i]->body.trimEnd().toStdString();
        if (targets[i]->size() > Patch::kMaxSectionBytes)
            return fail (PatchStatus::TooLarge, std::string ("the ") + names[i]
                                                    + " section is over 32 KB");
    }
    if (juce::String (p.voice).trim().isEmpty())
        return fail (PatchStatus::MissingVoice, "the ```eel voice block is empty");

    auto name = root.getProperty ("name", "").toString().trim();
    if (name.length() > kMaxNameChars)
        name = name.substring (0, kMaxNameChars).trim();
    p.name = name.isEmpty() ? std::string ("Untitled") : name.toStdString();

    const auto mode = root.getProperty ("voiceMode", "").toString().trim().toLowerCase();
    if (mode == "mono" || mode == "poly")
        p.voiceMode = mode.toStdString();
    else
        r.notes.push_back (mode.isEmpty() ? "no voiceMode, used poly" : "bad voiceMode, used poly");

    const juce::var macros = root.getProperty ("macros", juce::var());
    const auto* list = macros.getArray();
    if (list == nullptr && ! macros.isVoid())
        r.notes.push_back ("\"macros\" was not a list");
    const int given = list != nullptr ? list->size() : 0;
    for (int i = 0; i < given && i < Patch::kMaxMacros; ++i)
    {
        const juce::var& m = list->getReference (i);
        MacroSpec spec;
        juce::String label;
        double d = 0.5;
        if (m.isString())
        {
            label = m.toString();
        }
        else if (m.isObject())
        {
            label = m.getProperty ("label", "").toString();
            const juce::var def = m.getProperty ("default", juce::var());
            if (isNumber (def))
                d = static_cast<double> (def);
            else
                r.notes.push_back ("macro " + std::to_string (i + 1) + " has no numeric default, used 0.5");
        }
        label = label.trim();
        if (label.length() > kMaxLabelChars)
            label = label.substring (0, kMaxLabelChars).trim();
        spec.label = label.toStdString();
        if (! (d >= 0.0 && d <= 1.0))
        {
            const double c = d > 1.0 ? 1.0 : 0.0;
            r.notes.push_back ("clamped macro " + std::to_string (i + 1) + " default " + fmt (d)
                               + " -> " + fmt (c));
            d = c;
        }
        spec.defaultValue = quantisePatchNumber (d);
        p.macros.push_back (spec);
    }
    if (given > Patch::kMaxMacros)
        r.notes.push_back ("truncated macros " + std::to_string (given) + " -> 8");
    if (given < Patch::kMaxMacros)
    {
        r.notes.push_back ("padded macros " + std::to_string (given) + " -> 8");
        while ((int) p.macros.size() < Patch::kMaxMacros)
            p.macros.push_back (MacroSpec{});
    }

    const juce::var graph = root.getProperty ("graph", juce::var());
    if (graph.isObject())
        p.graphJson = juce::JSON::toString (graph, juce::JSON::FormatOptions{}
                                                       .withSpacing (juce::JSON::Spacing::none))
                          .toStdString();
    return r;
}

std::string formatPatchAsReply (const Patch& patch)
{
    // JSON-escape through juce::JSON so names and labels with quotes survive.
    auto quoted = [] (const std::string& s)
    {
        return juce::JSON::toString (juce::var (juce::String::fromUTF8 (s.c_str())), true).toStdString();
    };
    // 0.35, 0.123457, 1: up to 6 decimals (the .pulsar precision), no
    // trailing zeros, so the defaults read back exactly.
    auto number = [] (double v)
    {
        auto t = juce::String (v, 6);
        if (t.containsChar ('.'))
            t = t.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
        return t.toStdString();
    };
    std::string out = "```json\n{\n  \"name\": " + quoted (patch.name) + ",\n  \"voiceMode\": "
                    + quoted (patch.voiceMode) + ",\n  \"macros\": [\n";
    for (int i = 0; i < Patch::kMaxMacros; ++i)
    {
        const bool given = i < (int) patch.macros.size();
        const std::string label = given ? patch.macros[(size_t) i].label : std::string();
        out += "    {\"label\": " + quoted (label) + ", \"default\": "
             + number (patch.macroDefault (i)) + "}"
             + (i + 1 < Patch::kMaxMacros ? ",\n" : "\n");
    }
    out += "  ]";
    if (! patch.graphJson.empty())
        out += ",\n  \"graph\": " + patch.graphJson;
    out += "\n}\n```\n";
    out += "```eel init\n" + patch.init + (patch.init.empty() ? "" : "\n") + "```\n";
    out += "```eel voice\n" + patch.voice + "\n```\n";
    out += "```eel master\n" + patch.master + (patch.master.empty() ? "" : "\n") + "```\n";
    return out;
}

} // namespace vibesynth
