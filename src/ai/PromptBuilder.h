#pragma once

// Prompt construction (SPEC §6.1, §6.4) from the files in
// resources/prompts/ (embedded with JUCE BinaryData as PulsarPromptData):
//
//   system   system_prompt.md + api_reference.md (the whole lexicon, always)
//   2 few-shot pairs   user "<example prompt>" / assistant "<example reply>"
//                      from examples/*.md, picked by keyword (a bass prompt
//                      gets the bass example)
//   user     2-4 recipes from recipes/*.md picked by keyword (at least one
//            always; ties and padding in a fixed order, so the same prompt
//            always gets the same request) + the user's prompt
//
// A retry appends the model's previous reply (assistant) and the exact
// failure reason (user).
//
// Resource formats. A recipe starts with "# Title" and a "keywords:" line;
// an example has "category:", "keywords:" and "prompt:" lines, then a line
// "---", then the reply exactly as the model should write it.
//
// Compiled into the plugin and vibesynth_cli. Worker thread or the CLI.

#include <string>
#include <utility>
#include <vector>

#include "ai/MetaClient.h"

namespace vibesynth {

struct PromptRecipe
{
    std::string file;                  // "safe_feedback.md"
    std::string title;
    std::vector<std::string> keywords; // lower case
    std::string text;                  // the file without its keywords line
};

struct PromptExample
{
    std::string file;
    std::string category;              // "bass", "pluck", "texture"
    std::vector<std::string> keywords;
    std::string prompt;
    std::string reply;                 // the SPEC §6.2 response
};

// M11 (SPEC §9.7): one Mutate direction (edit/edit_directions.md).
struct MutateDirection
{
    std::string key;    // "darker"
    std::string label;  // "Darker" (the audition slot's caption)
    std::string text;   // what the model is told
};

struct PromptResources
{
    std::string systemPrompt;
    std::string apiReference;
    std::vector<PromptRecipe> recipes;   // in file-name order
    std::vector<PromptExample> examples; // in file-name order
    // M11: edit/edit_change.md and edit/edit_mutate.md (their "# Title"
    // line dropped; {node}, {instruction}, {direction} filled in by
    // buildEdit), and the 4 Mutate directions in file order.
    std::string changeInstructions;
    std::string mutateInstructions;
    std::vector<MutateDirection> mutateDirections;

    // The embedded files (parsed once, then shared).
    static const PromptResources& embedded();

    // (file name, text) of every embedded file, and the parse of such a
    // list (line endings normalised first; the smoketest feeds CRLF copies).
    static std::vector<std::pair<std::string, std::string>> embeddedFiles();
    static PromptResources fromFiles (std::vector<std::pair<std::string, std::string>> files);
};

struct RetryContext
{
    std::string previousReply;  // the model's last output, verbatim
    std::string reason;         // "compile error in voice: ..." / "too heavy: ..."
};

// M11 (SPEC §9.7): an edit of an existing patch. Change: `node` (a
// describeNodeForPrompt line) + `instruction`; Mutate: `direction` (a
// MutateDirection's text). `patchText` is formatPatchAsReply(active patch).
struct EditPrompt
{
    enum class Kind { Change, Mutate };
    Kind kind = Kind::Change;
    std::string patchText;
    std::string node;
    std::string instruction;
    std::string direction;
};

struct BuiltPrompt
{
    std::vector<ChatMessage> messages;
    std::vector<std::string> recipeFiles;   // which recipes were chosen
    std::vector<std::string> exampleFiles;  // which examples were chosen
};

class PromptBuilder
{
public:
    static constexpr int kMinRecipes = 2;
    static constexpr int kMaxRecipes = 4;
    static constexpr int kNumExamples = 2;

    explicit PromptBuilder (const PromptResources& resources = PromptResources::embedded())
        : res_ (resources) {}

    // `retry` null for the first attempt.
    BuiltPrompt build (const std::string& userPrompt, const RetryContext* retry = nullptr) const;

    // M11: the same system message as build() (so the service can reuse
    // its prefix), no few-shot pairs (the current patch shows the format),
    // then one user message: up to kMaxEditRecipes recipes picked by
    // keyword from the instruction or direction (at least one), "The
    // current patch:" + the patch, and the change or mutate block. A retry
    // appends the reply and reason exactly like build().
    static constexpr int kMaxEditRecipes = 2;
    BuiltPrompt buildEdit (const EditPrompt& edit, const RetryContext* retry = nullptr) const;

    // Keyword choices on their own (deterministic for a given prompt).
    std::vector<int> chooseRecipes (const std::string& userPrompt) const;
    std::vector<int> chooseExamples (const std::string& userPrompt) const;

    // True if the prompt reads as a bass sound (the batch renders those
    // with the riff phrase). Same keywords as the bass example.
    bool isBassPrompt (const std::string& userPrompt) const;

    // The final user message for a prompt ("Write a Pulsar patch for: ...").
    static std::string patchRequest (const std::string& userPrompt);

private:
    const PromptResources& res_;
};

// Rough token count (about 4 characters per token for English and code),
// for reporting request sizes when the service gives no usage.
int estimateTokens (const std::string& text) noexcept;

} // namespace vibesynth
