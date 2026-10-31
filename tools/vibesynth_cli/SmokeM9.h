#pragma once

// M9 smoketests (SPEC §9.5, §9.6): the validator's touched node list and
// the compile-only probe, the audio thread's meter harvest, the node
// layout, and the patch library (history pruning, user save). Offline;
// the library cases use a temporary folder. See SmokeM9.cpp.

namespace vibesynth_cli {

bool smokeTouchedNodes();
bool smokeMeterHarvest();
bool smokeNodeLayout();
bool smokePatchLibrary();

} // namespace vibesynth_cli
