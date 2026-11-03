// Prompt construction (SPEC §6.1, §6.4). See PromptBuilder.h.

#include "ai/PromptBuilder.h"

#include <juce_core/juce_core.h>

#include <algorithm>

#include "PulsarPromptData.h"
#include "ai/ResponseParser.h"
#include "patch/LexiconLint.h"
#include "patch/Patch.h"
#include "patch/PatchValidator.h"

namespace vibesynth {

namespace {

const std::vector<std::string> kReeseKeywords = { "reese", "detuned", "saws", "neuro", "rolling" };

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

// Lower case, anything but letters, digits, slashes and colons as spaces,
// padded with a space at each end, so " kw" finds a keyword at the start of a word.
std::string normalise (const std::string& text)
{
    std::string s = " ";
    for (const char c : text)
    {
        const char l = (c >= 'A' && c <= 'Z') ? static_cast<char> (c - 'A' + 'a') : c;
        s += ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9') || l == '/' || l == ':') ? l : ' ';
    }
    return s + " ";
}

std::vector<std::string> getMatchedWords (const std::string& normPrompt,
                                          const std::vector<std::string>& keywords,
                                          const std::vector<std::vector<std::string>>& synonymGroups)
{
    std::vector<std::string> matched;
    for (const auto& k : keywords)
    {
        const std::string nk = normalise (k);
        if (normPrompt.find (nk.substr (0, nk.size() - 1)) != std::string::npos)
        {
            if (std::find (matched.begin(), matched.end(), k) == matched.end())
                matched.push_back (k);
            continue;
        }
        for (const auto& group : synonymGroups)
        {
            if (std::find (group.begin(), group.end(), k) != group.end())
            {
                bool groupHit = false;
                for (const auto& syn : group)
                {
                    const std::string ns = normalise (syn);
                    if (normPrompt.find (ns.substr (0, ns.size() - 1)) != std::string::npos)
                    {
                        if (std::find (matched.begin(), matched.end(), syn) == matched.end())
                            matched.push_back (syn);
                        groupHit = true;
                        break;
                    }
                }
                if (groupHit)
                    break;
            }
        }
    }
    return matched;
}

int score (const std::string& normPrompt,
           const std::vector<std::string>& keywords,
           const std::vector<std::vector<std::string>>& synonymGroups)
{
    return static_cast<int> (getMatchedWords (normPrompt, keywords, synonymGroups).size());
}

} // namespace

std::string ExplainResult::print() const
{
    std::string s = "Prompt: \"" + userPrompt + "\"\n";
    s += "Tokens: ";
    for (size_t i = 0; i < promptTokens.size(); ++i)
        s += (i > 0 ? ", " : "") + promptTokens[i];
    s += "\n";
    if (! synonymExpansions.empty())
    {
        s += "Synonym expansions:\n";
        for (const auto& exp : synonymExpansions)
            s += "  " + exp + "\n";
    }
    s += "Examples (" + std::to_string (examples.size()) + "):\n";
    for (size_t i = 0; i < examples.size(); ++i)
    {
        s += "  Slot " + std::to_string (i + 1) + ": " + examples[i].file
             + " [" + examples[i].category + "] (score: " + std::to_string (examples[i].score)
             + ", " + examples[i].why + ")\n";
    }
    s += "Recipes matched (" + std::to_string (recipes.size()) + "):\n";
    for (size_t i = 0; i < recipes.size(); ++i)
    {
        s += "  " + std::to_string (i + 1) + ". " + recipes[i].file
             + " [" + recipes[i].category + "] (score: " + std::to_string (recipes[i].score)
             + ", " + recipes[i].why + ")\n";
    }
    s += "Padded recipes (" + std::to_string (paddedRecipes.size()) + "):\n";
    if (paddedRecipes.empty())
        s += "  (none)\n";
    else
    {
        for (size_t i = 0; i < paddedRecipes.size(); ++i)
            s += "  " + std::to_string (i + 1) + ". " + paddedRecipes[i].file
                 + " [" + paddedRecipes[i].category + "] (" + paddedRecipes[i].why + ")\n";
    }
    s += "Total prompt token estimate: ~" + std::to_string (totalEstimatedTokens) + " tokens\n";
    return s;
}

std::vector<std::pair<std::string, std::string>> PromptResources::embeddedFiles()
{
    std::vector<std::pair<std::string, std::string>> files;
    for (int i = 0; i < PulsarPromptData::namedResourceListSize; ++i)
        files.emplace_back (PulsarPromptData::originalFilenames[i], resourceText (i).toStdString());
    return files;
}

const PromptResources& PromptResources::embedded()
{
    static const PromptResources res = fromFiles (embeddedFiles());
    return res;
}

PromptResources PromptResources::fromFiles (std::vector<std::pair<std::string, std::string>> files)
{
    PromptResources r;
    std::sort (files.begin(), files.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [file, raw] : files)
    {
        const juce::String text (normaliseLineEndings (raw));
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
        if (file == "synonyms.md")
        {
            juce::StringArray synLines;
            synLines.addLines (text);
            for (const auto& l : synLines)
            {
                const auto trimmed = l.trim();
                if (trimmed.isEmpty() || trimmed.startsWith ("#"))
                    continue;
                r.synonymGroups.push_back (keywordList (":" + trimmed.toStdString()));
            }
            continue;
        }
        if (file.rfind ("edit_", 0) == 0)
        {
            // M11 edit prompts: drop the "# Title" line.
            const auto body = (text.trimStart().startsWith ("# ") ? text.fromFirstOccurrenceOf ("\n", false, false)
                                                                   : text).trim();
            if (file == "edit_change.md")
                r.changeInstructions = body.toStdString();
            else if (file == "edit_mutate.md")
                r.mutateInstructions = body.toStdString();
            else if (file == "edit_directions.md")
            {
                juce::StringArray lines;
                lines.addLines (body);
                for (const auto& l : lines)
                {
                    if (l.startsWith ("## "))
                    {
                        MutateDirection d;
                        d.key = l.substring (3).upToFirstOccurrenceOf (":", false, false).trim().toStdString();
                        d.label = l.fromFirstOccurrenceOf (":", false, false).trim().toStdString();
                        r.mutateDirections.push_back (d);
                    }
                    else if (! r.mutateDirections.empty() && l.trim().isNotEmpty())
                    {
                        auto& t = r.mutateDirections.back().text;
                        t += (t.empty() ? "" : " ") + l.trim().toStdString();
                    }
                }
            }
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
            else if (l.startsWith ("category:"))
                rec.category = l.fromFirstOccurrenceOf (":", false, false).trim().toStdString();
            else
                body.add (l);
            if (l.startsWith ("# ") && rec.title.empty())
                rec.title = l.substring (2).trim().toStdString();
        }
        rec.text = body.joinIntoString ("\n").trim().toStdString();
        r.recipes.push_back (rec);
    }
    return r;
}

std::vector<int> PromptBuilder::chooseRecipes (const std::string& userPrompt, const std::string& firstExampleCategory) const
{
    const std::string norm = normalise (userPrompt);
    std::string firstCat = firstExampleCategory;
    if (firstCat.empty())
    {
        const auto exIndices = chooseExamples (userPrompt);
        if (! exIndices.empty() && exIndices[0] >= 0 && exIndices[0] < (int) res_.examples.size())
            firstCat = res_.examples[(size_t) exIndices[0]].category;
    }

    const std::vector<std::string> kMovementWords = {
        "wobble", "wub", "lfo", "rate", "rhythmic", "chop", "gate", "stutter", "1/4", "1/8", "1/16", "sync"
    };
    const std::vector<std::string> kOttWords = {
        "ott", "squash", "loud", "compressed"
    };
    const bool tempoSyncAllowed = score (norm, kMovementWords, res_.synonymGroups) > 0;
    const bool ottAllowed = isBassPrompt (userPrompt) || score (norm, kOttWords, res_.synonymGroups) > 0;

    std::vector<std::pair<int, int>> scored;
    for (int i = 0; i < (int) res_.recipes.size(); ++i)
    {
        const auto& r = res_.recipes[(size_t) i];
        if (r.file == "tempo_sync.md" && ! tempoSyncAllowed)
            continue;
        if (r.file == "ott_glue.md" && ! ottAllowed)
            continue;
        const int s = score (norm, r.keywords, res_.synonymGroups);
        if (s > 0)
            scored.emplace_back (-s, i);
    }
    std::stable_sort (scored.begin(), scored.end());

    std::vector<int> out;
    for (const auto& p : scored)
        out.push_back (p.second);

    // When fewer than 2 recipes match, pad only from recipes whose category matches
    // the chosen first example.
    if ((int) out.size() < 2 && ! firstCat.empty())
    {
        for (int i = 0; i < (int) res_.recipes.size() && (int) out.size() < 2; ++i)
        {
            if (std::find (out.begin(), out.end(), i) != out.end())
                continue;
            const auto& r = res_.recipes[(size_t) i];
            if (r.category != firstCat)
                continue;
            if (r.file == "tempo_sync.md" && ! tempoSyncAllowed)
                continue;
            if (r.file == "ott_glue.md" && ! ottAllowed)
                continue;
            out.push_back (i);
        }
    }

    if ((int) out.size() > kMaxRecipes)
        out.resize ((size_t) kMaxRecipes);

    return out;
}

std::vector<int> PromptBuilder::chooseExamples (const std::string& userPrompt) const
{
    const std::string norm = normalise (userPrompt);
    const bool allowReese = score (norm, kReeseKeywords, res_.synonymGroups) > 0;

    int bestPadIdx = -1;
    int bestFmIdx = -1;
    for (int i = 0; i < (int) res_.examples.size(); ++i)
    {
        if (res_.examples[(size_t) i].file == "example_bloom_drift.md")
            bestPadIdx = i;
        if (res_.examples[(size_t) i].file == "example_sine_wub.md")
            bestFmIdx = i;
    }
    if (bestPadIdx < 0 && ! res_.examples.empty()) bestPadIdx = 0;
    if (bestFmIdx < 0 && ! res_.examples.empty()) bestFmIdx = (int) res_.examples.size() > 1 ? 1 : 0;

    // Slot 2 when nothing else matched: a bass prompt gets a second bass of
    // another kind (square, else FM; never the reese unless asked), anything
    // else gets the pad (or the FM wub after a pad).
    auto neutralSecond = [&] (const std::string& cat1) -> int
    {
        if (cat1.rfind ("bass-", 0) == 0)
        {
            for (const char* want : { "bass-square", "bass-fm" })
                for (int i = 0; i < (int) res_.examples.size(); ++i)
                    if (res_.examples[(size_t) i].category == want && cat1 != want)
                        return i;
        }
        return cat1 == "pad" ? bestFmIdx : bestPadIdx;
    };

    std::vector<std::pair<int, int>> scored;
    for (int i = 0; i < (int) res_.examples.size(); ++i)
    {
        const auto& e = res_.examples[(size_t) i];
        if (! allowReese && (e.category == "bass-reese" || e.file == "example_bass.md"))
            continue;
        const int s = score (norm, e.keywords, res_.synonymGroups);
        if (s > 0)
            scored.emplace_back (-s, i);
    }
    std::stable_sort (scored.begin(), scored.end());

    std::vector<int> out;
    if (scored.empty())
    {
        // No example matched. If a recipe did ("acid", "ocean waves"), take
        // the first example in that recipe's category; recipes with no
        // example of their own borrow the nearest one.
        int bestRecipe = -1, bestRecipeScore = 0;
        for (int i = 0; i < (int) res_.recipes.size(); ++i)
        {
            const int s = score (norm, res_.recipes[(size_t) i].keywords, res_.synonymGroups);
            if (s > bestRecipeScore)
            {
                bestRecipeScore = s;
                bestRecipe = i;
            }
        }
        int byRecipe = -1;
        if (bestRecipe >= 0)
        {
            std::string cat = res_.recipes[(size_t) bestRecipe].category;
            if (cat == "bass-sub")
                cat = "bass-fm";  // 808 glide, noise bass: the clean sub example
            if (cat == "bass-reese" && ! allowReese)
                cat = "bass-square";  // "brostep growl": a bass, not the pad
            for (int i = 0; i < (int) res_.examples.size() && byRecipe < 0; ++i)
            {
                const auto& e = res_.examples[(size_t) i];
                if (e.category == cat && (allowReese || e.category != "bass-reese"))
                    byRecipe = i;
            }
        }
        if (byRecipe >= 0)
        {
            out.push_back (byRecipe);
            out.push_back (neutralSecond (res_.examples[(size_t) byRecipe].category));
            return out;
        }
        out.push_back (bestPadIdx);
        out.push_back (bestFmIdx);
        return out;
    }

    const int slot1Idx = scored[0].second;
    out.push_back (slot1Idx);
    const std::string& cat1 = res_.examples[(size_t) slot1Idx].category;

    // Slot 2: best match from a different category
    int slot2Idx = -1;
    for (size_t k = 1; k < scored.size(); ++k)
    {
        const int candIdx = scored[k].second;
        if (res_.examples[(size_t) candIdx].category != cat1)
        {
            slot2Idx = candIdx;
            break;
        }
    }
    if (slot2Idx < 0)
    {
        // Neutral fallback from a different category
        slot2Idx = neutralSecond (cat1);
    }
    out.push_back (slot2Idx);
    return out;
}

std::vector<PromptExample> PromptBuilder::selectExamples (const std::string& userPrompt,
                                                         const std::vector<PromptExample>& userExamples) const
{
    const std::string norm = normalise (userPrompt);
    const auto builtinIndices = chooseExamples (userPrompt);
    std::vector<PromptExample> out;
    for (int idx : builtinIndices)
        out.push_back (res_.examples[(size_t) idx]);

    if (userExamples.empty() || out.empty())
        return out;

    // Check if any user example scores strictly higher than the best built-in example
    const int bestBuiltinScore = score (norm, out[0].keywords, res_.synonymGroups);
    int bestUserScore = 0;
    int bestUserIdx = -1;
    for (int i = 0; i < (int) userExamples.size(); ++i)
    {
        const int s = score (norm, userExamples[(size_t) i].keywords, res_.synonymGroups);
        if (s > bestUserScore)
        {
            bestUserScore = s;
            bestUserIdx = i;
        }
    }

    if (bestUserIdx >= 0 && bestUserScore > bestBuiltinScore)
    {
        if (out.size() >= 2)
            out[1] = userExamples[(size_t) bestUserIdx];
        else
            out.push_back (userExamples[(size_t) bestUserIdx]);
    }

    return out;
}

ExplainResult PromptBuilder::explainPrompt (const std::string& userPrompt,
                                           const std::vector<PromptExample>& userExamples) const
{
    ExplainResult res;
    res.userPrompt = userPrompt;
    const std::string norm = normalise (userPrompt);

    // Tokens
    juce::StringArray tokens;
    tokens.addTokens (juce::String (userPrompt).toLowerCase(), " \t\r\n.,;:!?-/()[]\"'`", "");
    tokens.trim();
    tokens.removeEmptyStrings();
    for (const auto& t : tokens)
    {
        const std::string ts = t.toStdString();
        if (std::find (res.promptTokens.begin(), res.promptTokens.end(), ts) == res.promptTokens.end())
            res.promptTokens.push_back (ts);
    }

    // Synonym expansions
    for (const auto& token : res.promptTokens)
    {
        for (const auto& group : res_.synonymGroups)
        {
            if (std::find (group.begin(), group.end(), token) != group.end())
            {
                std::string synStr = token + " -> ";
                int added = 0;
                for (const auto& syn : group)
                {
                    if (syn == token) continue;
                    synStr += (added > 0 ? ", " : "") + syn;
                    ++added;
                }
                if (added > 0 && std::find (res.synonymExpansions.begin(), res.synonymExpansions.end(), synStr) == res.synonymExpansions.end())
                    res.synonymExpansions.push_back (synStr);
            }
        }
    }

    const auto examples = selectExamples (userPrompt, userExamples);
    for (size_t i = 0; i < examples.size(); ++i)
    {
        const auto& e = examples[i];
        ExplainItem item;
        item.file = e.file;
        item.category = e.category;
        item.matchedWords = getMatchedWords (norm, e.keywords, res_.synonymGroups);
        item.score = static_cast<int> (item.matchedWords.size());
        if (! item.matchedWords.empty())
        {
            std::string w = "matched: ";
            for (size_t k = 0; k < item.matchedWords.size(); ++k)
                w += (k > 0 ? ", " : "") + item.matchedWords[k];
            item.why = w;
        }
        else if (e.category == "user")
        {
            item.why = "user preset (strictly higher score than built-in)";
        }
        else
        {
            item.why = (i == 0) ? "no example matched: from the best recipe's category, else the neutral pad"
                                : "different category fallback (slot 2)";
        }
        res.examples.push_back (item);
    }

    const std::string firstCat = examples.empty() ? "" : examples[0].category;
    const auto recipeIndices = chooseRecipes (userPrompt, firstCat);
    for (int idx : recipeIndices)
    {
        const auto& r = res_.recipes[(size_t) idx];
        ExplainItem item;
        item.file = r.file;
        item.category = r.category;
        item.matchedWords = getMatchedWords (norm, r.keywords, res_.synonymGroups);
        item.score = static_cast<int> (item.matchedWords.size());
        if (! item.matchedWords.empty())
        {
            std::string w = "matched: ";
            for (size_t k = 0; k < item.matchedWords.size(); ++k)
                w += (k > 0 ? ", " : "") + item.matchedWords[k];
            item.why = w;
            res.recipes.push_back (item);
        }
        else
        {
            item.why = "padded from first example category: " + firstCat;
            res.paddedRecipes.push_back (item);
        }
    }

    const BuiltPrompt built = build (userPrompt, nullptr, "", userExamples);
    int total = 0;
    for (const auto& m : built.messages)
        total += estimateTokens (m.content);
    res.totalEstimatedTokens = total;

    return res;
}

std::vector<PromptExample> PromptBuilder::loadUserPresetExamples (const std::string& folderPath)
{
    std::vector<PromptExample> out;
    const juce::File folder (folderPath);
    if (! folder.isDirectory())
        return out;

    for (const auto& file : folder.findChildFiles (juce::File::findFiles, false, "*.pulsar"))
    {
        if (file.getSize() > 10 * 1024)
            continue;
        const std::string text = file.loadFileAsString().toStdString();
        const ParseResult pr = parsePatch (text);
        if (! pr.ok())
            continue;
        const auto& p = pr.patch;
        if (p.init.size() + p.voice.size() + p.master.size() > 10 * 1024)
            continue;
        const LintResult lr = lintPatch (p);
        if (! lr.ok())
            continue;

        PromptExample e;
        e.file = file.getFileName().toStdString();
        e.category = "user";

        // Clean prompt: strip any lines starting with mutate:
        juce::StringArray plines;
        plines.addLines (p.prompt);
        juce::String cleanPrompt;
        for (const auto& pl : plines)
        {
            if (pl.trim().startsWithIgnoreCase ("mutate:"))
                continue;
            if (cleanPrompt.isNotEmpty()) cleanPrompt << " ";
            cleanPrompt << pl.trim();
        }
        e.prompt = cleanPrompt.trim().toStdString();
        if (e.prompt.empty())
            e.prompt = p.name;

        // Keywords from name, cleaned prompt, macro labels
        std::string rawKw = p.name + " " + e.prompt;
        for (const auto& m : p.macros)
            rawKw += " " + m.label;

        juce::StringArray kwTokens;
        kwTokens.addTokens (juce::String (normalise (rawKw)), " ", "");
        kwTokens.trim();
        kwTokens.removeEmptyStrings();
        for (const auto& kw : kwTokens)
        {
            const std::string s = kw.toLowerCase().toStdString();
            if (std::find (e.keywords.begin(), e.keywords.end(), s) == e.keywords.end())
                e.keywords.push_back (s);
        }

        e.reply = formatPatchAsReply (p);
        out.push_back (e);
    }
    return out;
}

bool PromptBuilder::isBassPrompt (const std::string& userPrompt) const
{
    const std::string norm = normalise (userPrompt);
    for (const auto& e : res_.examples)
        if (e.category.rfind ("bass", 0) == 0 && score (norm, e.keywords, res_.synonymGroups) > 0)
            return true;
    return false;
}

std::string PromptBuilder::patchRequest (const std::string& userPrompt)
{
    return "Write a Pulsar patch for: " + userPrompt;
}

BuiltPrompt PromptBuilder::build (const std::string& userPrompt, const RetryContext* retry,
                                  const std::string& extraUserLine,
                                  const std::vector<PromptExample>& userExamples) const
{
    BuiltPrompt b;
    b.messages.push_back ({ "system", res_.systemPrompt + "\n\n" + res_.apiReference });

    const auto examples = selectExamples (userPrompt, userExamples);
    for (const auto& e : examples)
    {
        b.messages.push_back ({ "user", patchRequest (e.prompt) });
        b.messages.push_back ({ "assistant", e.reply });
        b.exampleFiles.push_back (e.file);
    }

    const std::string firstCat = examples.empty() ? "" : examples[0].category;
    std::string user = "Techniques that may help (adapt them, don't paste them):";
    for (const int i : chooseRecipes (userPrompt, firstCat))
    {
        user += "\n\n" + res_.recipes[(size_t) i].text;
        b.recipeFiles.push_back (res_.recipes[(size_t) i].file);
    }
    user += "\n\n" + patchRequest (userPrompt);
    if (! extraUserLine.empty())
        user += "\n" + extraUserLine;
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

BuiltPrompt PromptBuilder::buildEdit (const EditPrompt& edit, const RetryContext* retry) const
{
    BuiltPrompt b;
    b.messages.push_back ({ "system", res_.systemPrompt + "\n\n" + res_.apiReference });

    const bool change = edit.kind == EditPrompt::Kind::Change;
    std::vector<int> recipes = chooseRecipes (change ? edit.instruction : edit.direction);
    if ((int) recipes.size() > kMaxEditRecipes)
        recipes.resize ((size_t) kMaxEditRecipes);
    std::string user = "Techniques that may help (adapt them, don't paste them):";
    for (const int i : recipes)
    {
        user += "\n\n" + res_.recipes[(size_t) i].text;
        b.recipeFiles.push_back (res_.recipes[(size_t) i].file);
    }
    user += "\n\nThe current patch:\n\n" + edit.patchText + "\n";

    juce::String block (change ? res_.changeInstructions : res_.mutateInstructions);
    block = block.replace ("{node}", juce::String (edit.node))
                 .replace ("{instruction}", juce::String (edit.instruction))
                 .replace ("{direction}", juce::String (edit.direction));
    user += block.toStdString();
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
