#pragma once

// M10 (SPEC §9.6): node inspector tests and vibesynth_cli --bake. See
// SmokeM10.cpp and Bake.cpp.

namespace vibesynth_cli {

// All M10 smoketests: lexicon metadata, node ids, override maths, bypass
// per kind, stateless numbering, tweak persistence (.pulsar and plugin
// state), traces, bake refusals, bake equivalence on every built-in with
// seeded random tweaks, and bit-identical renders with no overrides.
bool smokeM10();

// --bake in.pulsar out.pulsar: folds the tweaks into the code, runs the
// offline test on the result (its trim is the computed one) and writes it.
// 0 ok, 1 refused or rejected, 2 bad input.
int runBake (const char* inPath, const char* outPath, int oversampling);

} // namespace vibesynth_cli
