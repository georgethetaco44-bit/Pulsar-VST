#pragma once

// M11 smoketests (SPEC §9.7), offline (canned replies, no network): node
// ids, the patch-as-reply round trip, the edit prompt resources, "Change
// this node" (request contents, kept slots, a refused node), Mutate (4
// canned slots with one failure and one 429, one test at a time, the
// sequential fallback, a persistent 429), the undo stack, and Cancel.
// See SmokeM11.cpp.

namespace vibesynth_cli {

bool smokeM11();

} // namespace vibesynth_cli
