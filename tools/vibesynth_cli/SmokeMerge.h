#pragma once

// M10 + M11 merge smoketests (SPEC §9.6, §9.7), offline (canned replies,
// no network): every built-in's node ids round-trip through the one
// node-id implementation; Undo brings back tweaks and macro maps bit for
// bit; "Change this node" keeps the tweaks whose node survived and names
// the rest; Mutate sends the tweaks baked into the code (or the unbaked
// code with a note when bake refuses); Discard restores the tweaks. See
// SmokeMerge.cpp.

namespace vibesynth_cli {

bool smokeMerge();

} // namespace vibesynth_cli
