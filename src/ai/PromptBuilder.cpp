// Prompt construction (SPEC §6.1, §6.4). See PromptBuilder.h.

#include "ai/PromptBuilder.h"

#include <juce_core/juce_core.h>

#include <algorithm>

#include "PulsarPromptData.h"

namespace vibesynth {

namespace {

// Padding when fewer than kMinRecipes match: glue and tempo motion suit
// almost any sound.
const char* const kDefaultRecipes[] = { "ott_glue.md", "tempo_sync.md" };

juce::String resourceText (int i)
{
    int size = 0;
    const char* data = PulsarPromptData::getNamedResource (PulsarPromptData::namedResourceList[i], size);
    return data != nullptr ? juce::String::fromUTF8 (data, size) : juce::String();
}

std::vector<std::string> keywordList (const juce::String& line)
{
    juce::StringArray t;
    t.addTokens (line.fromFirstOccurrenceOf (":", false, false), ",", "");
    t.trim();
    t.removeEmptyStrings();
    std::vector<std::string> out;
    for (const auto& k : t)
        out.push_back (k.toLowerCase().toStdString());
    return out;
}

// Lower case, anything but letters and digits as spaces, padded with a
// space at each end, so " kw" finds a keyword at the start of a word.
std::string normalise (const std::string& text)
{
    std::string s = " ";
    for (const char c : text)
    {
        const char l = (c >= 'A' && c <= 'Z') ? static_cast<char> (c - 'A' + 'a') : c;
        s += ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9')) ? l : ' ';
    }
    return s + " ";
}

int score (const std::string& normPrompt, const std::vector<std::string>& keywords)
{
    int n = 0;
    for (const auto& k : keywords)
    {
        const std::string nk = normalise (k);
        // " kw " without its trailing space: a word-prefix match ("wobbl"
        // finds "wobbly").
        if (normPrompt.find (nk.substr (0, nk.size() - 1)) != std::string::npos)
            ++n;
    }
    return n;
}

// Indices with a positive score, best first (ties in file order).
std::vector<int> ranked (const std::string& prompt, const std::vector<std::vector<std::string>>& kws)
{
    const std::string norm = normalise (prompt);
    std::vector<std::pair<int, int>> s;
    for (int i = 0; i < (int) kws.size(); ++i)
        s.emplace_back (-score (norm, kws[(size_t) i]), i);
    std::stable_sort (s.begin(), s.end());
    std::vector<int> out;
    for (const auto& p : s)
        if (p.first < 0)
            out.push_back (p.second);
    return out;
}

} // namespace

const PromptResources& PromptResources::embedded()
{
    static const PromptResources res = []
    {
        PromptResources r;
        std::vector<std::pair<std::string, juce::String>> files;
        for (int i = 0; i < PulsarPromptData::namedResourceListSize; ++i)
            files.emplace_back (PulsarPromptData::originalFilenames[i], resourceText (i));
        std::sort (files.begin(), files.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });

        for (const auto& [file, text] : files)
        {
            if (file == "system_prompt.md")
            {
                r.systemPrompt = text.trim().toStdString();
                continue;
            }
            if (file == "api_reference.md")
            {
                r.apiReference = text.trim().toStdString();
                continue;
            }
            juce::StringArray lines;
            lines.addLines (text);
            if (lines.size() > 0 && lines[0].startsWith ("category:"))
            {
                PromptExample e;
                e.file = file;
                int i = 0;
                for (; i < lines.size() && lines[i].trim() != "---"; ++i)
                {
                    const auto& l = lines[i];
                    if (l.startsWith ("category:"))
                        e.category = l.fromFirstOccurrenceOf (":", false, false).trim().toStdString();
                    else if (l.startsWith ("keywords:"))
                        e.keywords = keywordList (l);
                    else if (l.startsWith ("prompt:"))
                        e.prompt = l.fromFirstOccurrenceOf (":", false, false).trim().toStdString();
                }
                juce::StringArray body;
                for (++i; i < lines.size(); ++i)
                    body.add (lines[i]);
                e.reply = body.joinIntoString ("\n").trim().toStdString();
                r.examples.push_back (e);
                continue;
            }
            PromptRecipe rec;
            rec.file = file;
            juce::StringArray body;
            for (const auto& l : lines)
            {
                if (l.startsWith ("keywords:"))
                    rec.keywords = keywordList (l);
                else
                    body.add (l);
                if (l.startsWith ("# ") && rec.title.empty())
                    rec.title = l.substring (2).trim().toStdString();
            }
            rec.text = body.joinIntoString ("\n").trim().toStdString();
            r.recipes.push_back (rec);
        }
        return r;
    }();
    return res;
}

std::vector<int> PromptBuilder::chooseRecipes (const std::string& userPrompt) const
{
    std::vector<std::vector<std::string>> kws;
    for (const auto& r : res_.recipes)
        kws.push_back (r.keywords);
    std::vector<int> out = ranked (userPrompt, kws);
    if ((int) out.size() > kMaxRecipes)
        out.resize ((size_t) kMaxRecipes);
    auto add = [&out] (int i)
    {
        if (i >= 0 && std::find (out.begin(), out.end(), i) == out.end())
            out.push_back (i);
    };
    for (const char* file : kDefaultRecipes)
        for (int i = 0; i < (int) res_.recipes.size() && (int) out.size() < kMinRecipes; ++i)
            if (res_.recipes[(size_t) i].file == file)
                add (i);
    for (int i = 0; i < (int) res_.recipes.size() && (int) out.size() < kMinRecipes; ++i)
        add (i);
    return out;
}

std::vector<int> PromptBuilder::chooseExamples (const std::string& userPrompt) const
{
    std::vector<std::vector<std::string>> kws;
    for (const auto& e : res_.examples)
        kws.push_back (e.keywords);
    std::vector<int> out = ranked (userPrompt, kws);
    for (int i = 0; i < (int) res_.examples.size(); ++i)
        if (std::find (out.begin(), out.end(), i) == out.end())
            out.push_back (i);
    if ((int) out.size() > kNumExamples)
        out.resize ((size_t) kNumExamples);
    return out;
}

bool PromptBuilder::isBassPrompt (const std::string& userPrompt) const
{
    const std::string norm = normalise (userPrompt);
    for (const auto& e : res_.examples)
        if (e.category == "bass")
            return score (norm, e.keywords) > 0;
    return false;
}

std::string PromptBuilder::patchRequest (const std::string& userPrompt)
{
    return "Write a Pulsar patch for: " + userPrompt;
}

BuiltPrompt PromptBuilder::build (const std::string& userPrompt, const RetryContext* retry) const
{
    BuiltPrompt b;
    b.messages.push_back ({ "system", res_.systemPrompt + "\n\n" + res_.apiReference });

    for (const int i : chooseExamples (userPrompt))
    {
        const auto& e = res_.examples[(size_t) i];
        b.messages.push_back ({ "user", patchRequest (e.prompt) });
        b.messages.push_back ({ "assistant", e.reply });
        b.exampleFiles.push_back (e.file);
    }

    std::string user = "Techniques that may help (adapt them, don't paste them):";
    for (const int i : chooseRecipes (userPrompt))
    {
        user += "\n\n" + res_.recipes[(size_t) i].text;
        b.recipeFiles.push_back (res_.recipes[(size_t) i].file);
    }
    user += "\n\n" + patchRequest (userPrompt);
    b.messages.push_back ({ "user", user });

    if (retry != nullptr)
    {
        b.messages.push_back ({ "assistant", retry->previousReply });
        b.messages.push_back ({ "user", "Pulsar rejected that patch: " + retry->reason
                                            + "\nFix exactly this and reply with the complete patch again in the "
                                              "same format (the ```json header, then the ```eel init, ```eel voice "
                                              "and ```eel master blocks)." });
    }
    return b;
}

int estimateTokens (const std::string& text) noexcept
{
    return static_cast<int> ((text.size() + 3) / 4);
}

} // namespace vibesynth
