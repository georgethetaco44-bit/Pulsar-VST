#pragma once

// M4: the hardcoded PatchStrings struct (SPEC §4.2: three code sections).
// M5 replaces the source of these strings with .vibepatch files; the
// execution machinery stays exactly the same.
//
// Test preset: reese-style bass (SPEC §8 recipes). Macro map:
//   m1 = detune, m2 = cutoff, m3 = resonance/feedback depth, m4 = drive.
// Uses 11 lexicon functions: osc_supersaw, osc_saw, osc_sine (sub),
// flt_lp, env_adsr, fx_sat, fx_fold, util_safe_fb, bus_mono_low(+_r),
// bus_sat.
//
// The filter is flt_lp, not flt_ladder: with the ladder (DaisySP runs it
// 4x oversampled internally) the voice measured ~400 ns on the Deck, over
// the SPEC §4.2.1 300 ns budget; vibesynth_cli --bench still reports the
// ladder variant.

#include <cstdint>

namespace vibesynth {

struct PatchStrings
{
    const char* init;
    const char* voice;
    const char* master;
};

// Release r = 0.5 → ~100 ms under the 1 ms–10 s exponential map
// (amendment 3; 16 ms was too short for a bass tail).
inline const PatchStrings kTestPreset = {
    "fb1 = 0; prev = 0;",
    "det = 1 + (m1 - 0.5) * 0.02;"
    "a = osc_supersaw(0, freq * det * (1 + fb1 * m3 * 0.02), m1, 0.65);"
    "b = osc_saw(0, freq * (2 - det));"
    "sub = osc_sine(1, freq * 0.5);"
    "env = env_adsr(0, gate, 0.08, 0.35, 0.55, 0.5);"
    "cut = min(0.97, 0.12 + m2 * 0.55 + env * 0.30);"
    "x = a * 0.7 + b * 0.3;"
    "y = flt_lp(0, x, cut, m3);"
    "y = fx_sat(y + sub * 0.5, m4);"
    "y = fx_fold(y, m4 * 0.35);"
    "fb1 = util_safe_fb(0, y);"
    "out = y * env * (0.5 + 0.5 * vel);",
    "outl = bus_mono_low(inl, inr);"
    "outr = bus_mono_low_r();"
    "outl = bus_sat(outl, 0.35);"
    "outr = bus_sat(outr, 0.35);"
};

} // namespace vibesynth
