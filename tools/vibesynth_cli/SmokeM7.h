#pragma once

// M7 smoketests (SPEC §4.2, §4.2.1, §9.1): the 2x path's decimator, the
// fold aliasing A/B, the own filters, and the voice modes. See SmokeM7.cpp.

namespace vibesynth_cli {

bool smokeDecimator();
bool smokeAliasAB();
bool smokeFilters();
bool smokeVoiceModes();

} // namespace vibesynth_cli
