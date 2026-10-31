#pragma once

// Lexicon lint (SPEC §6.4, M8 additions), run by the validator before
// anything is compiled, for generated and loaded patches alike. Every
// `name(` call in every section is checked against the registered lexicon
// (engine/LexiconTable.h), the EEL2 built-ins Pulsar allows, and the
// patch's own `function` definitions:
//   - unknown names fail with the closest valid names and the function
//     family ("unknown function `osc_wavetable`; did you mean `osc_saw`?
//     osc_* are: ..."), because invented functions are the most common
//     generation failure;
//   - argument counts must match exactly (EEL2 only checks the minimum);
//   - a literal slot must be an integer inside the function's pool;
//   - bus_* functions are master-only;
//   - built-ins that free memory or use the VM stack are refused.
//
// Like the loop-nesting check this is a scanner, not a parser: comments
// and strings are blanked first, then identifiers followed by `(` are
// calls. JUCE-free, part of vibesynth_engine. Never on the audio thread.

#include <string>
#include <vector>

#include "patch/Patch.h"

namespace vibesynth {

struct LintResult
{
    // One line per problem, e.g. "voice line 7: osc_saw takes 2 arguments
    // (slot, hz), got 3". At most kMaxIssues.
    std::vector<std::string> issues;
    static constexpr int kMaxIssues = 6;

    bool ok() const noexcept { return issues.empty(); }
    // All issues joined with "; " (the reject reason and self-heal text).
    std::string message() const;
};

LintResult lintPatch (const Patch& patch);

// One section on its own (smoketest cases). `section` is "init", "voice"
// or "master" and decides where bus_* may appear.
LintResult lintSection (const std::string& code, const std::string& section);

// The EEL2 built-ins a patch may call, with their argument counts (for
// the api_reference.md smoketest). maxArgs < 0 means "or more".
struct EelBuiltin
{
    const char* name;
    int minArgs;
    int maxArgs;
};
int numAllowedBuiltins() noexcept;
const EelBuiltin& allowedBuiltin (int index) noexcept;

} // namespace vibesynth
