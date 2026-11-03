#pragma once

// M14b smoketests (SPEC §9.6, §9.7):
//   - Knob value parser table (units, clamping, garbage rejection)
//   - Steered mutate directions (N=1..5, custom text, intensity spread)
//   - Auto mutate chips (exact lit chips in order)
//   - Old CLI mutate form unchanged (4 default directions)
//   - Card save and replace offline validation
//   - Typed value landing and undo restoration

namespace vibesynth_cli {

bool smokeM14b();

} // namespace vibesynth_cli
