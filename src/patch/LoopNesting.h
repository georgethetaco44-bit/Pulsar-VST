#pragma once

// Loop-nesting text check (SPEC §7.1, runaway code), run before any
// section is compiled. With EEL2's 1024-iteration cap per loop, two levels
// of nesting bound one sample at ~1M iterations (a few ms), so the
// validator's per-sample deadline always fires promptly; a third level
// would make one sample take seconds.
//
// The check is a small conservative scanner, not a parser: comments,
// "strings" and 'char' constants are blanked first, then parentheses are
// matched. A `loop(` or `while(` group is one level (as is a paren group
// directly after a `while(...)`, the while-body form). A call to a user
// function counts that function's own nesting at the call site; functions
// from all sections are visible everywhere and recursion counts as too
// deep. Over-counting (a false reject) is acceptable; under-counting is not.
//
// JUCE-free, part of vibesynth_engine. Never runs on the audio thread.

#include <string>

#include "patch/Patch.h"

namespace vibesynth {

constexpr int kMaxLoopNesting = 2;

struct LoopNestingResult
{
    int depth = 0;        // deepest nesting found (over all sections)
    std::string section;  // "init", "voice" or "master" where it was found

    bool ok() const noexcept { return depth <= kMaxLoopNesting; }
};

LoopNestingResult checkLoopNesting (const Patch& patch);

// Single-section form (smoketest cases): functions defined in `code` only.
int loopNestingDepth (const std::string& code);

// "loops nested too deep (max 2) in voice" (empty when r.ok()).
std::string loopNestingError (const LoopNestingResult& r);

} // namespace vibesynth
