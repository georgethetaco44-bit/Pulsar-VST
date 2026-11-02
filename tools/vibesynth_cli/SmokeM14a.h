#pragma once

namespace vibesynth_cli {

// M14a smoketests:
//   - macro-scan determinism (Wiener entropy / STFT spectral flatness)
//   - validator noise flag (rejects synthetic noise-at-1, accepts noisy textures, gated)
//   - macroRanges JSON serialization / deserialization round-trip
//   - macro range bake equivalence (bit-identical renders)
//   - macro range undo / discard restoration
//   - fx_comp and fx_gate (bounds/NaN, compression/gating, modulation, bake)
//   - bus_delay 6th arg (lowcut frequency response)
//   - phrase riff octave transposition
bool smokeM14a();

} // namespace vibesynth_cli
