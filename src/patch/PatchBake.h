#pragma once

// M10 (SPEC §9.6): vibesynth_cli --bake. Folds a patch's tweaks and macro
// maps into its code, so a hand-shaped preset can become an LLM example.
// JUCE-free, part of vibesynth_engine.
//
// Call sites are found by scanning each section with paren matching
// (comments and strings skipped); slots are literals (SPEC §5.1).
// Stateless shapers are numbered as the engine numbers them: per module,
// in the order calls complete (arguments first, left to right), from 0 in
// each of init, voice and master, the 4th and later sharing #3. A tweaked
// stateless module whose calls sit in a branch (? : && ||), a loop/while
// or a user function has no fixed numbering, so bake refuses it.
//
// Per call, with the node's overrides (the same OverrideTable maths the
// engine uses, so the literals are the engine's exact doubles):
//   offset   min(max((expr) + k, lo), hi), or (expr) * f for hz
//   replace  the constant
//   macro    (lo + mN * d), d = hi - lo
//   bypass   the passed argument, 0, or the forced argument (mix 0)
// An argument that is dropped but may have side effects (a call or an
// assignment) is kept as a statement before the result, so stateful calls
// inside it still run in the same order. Unused macros that gain a map are
// labelled with the argument's name.

#include <string>
#include <vector>

#include "patch/Patch.h"

namespace vibesynth {

struct BakeResult
{
    bool ok = false;
    std::string error;              // why the bake was refused
    std::vector<std::string> notes; // tweaks with no call site, labels set
    Patch patch;                    // the baked patch (no tweaks or maps); trim unchanged
    int callsChanged = 0;
};

BakeResult bakePatch (const Patch& patch);

// A number as EEL2 parses it back exactly (no exponent; negative values
// in parentheses).
std::string formatEelNumber (double v);

} // namespace vibesynth
