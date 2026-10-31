#pragma once

// M10 (SPEC §9.6): the bridge between a Patch's saved tweaks
// and the processor's OverrideTable. JUCE-free, part of vibesynth_engine.
//
// Node ids ("flt_lp:0", "fx_sat#1", "master/bus_reverb") are ai/NodeId.h's,
// the one implementation (NodeRef, parseNodeId, nodeIdString).

#include <string>
#include <vector>

#include "ai/NodeId.h"
#include "engine/NodeOverrides.h"
#include "patch/Patch.h"

namespace vibesynth {

// Makes `table` hold exactly the patch's tweaks and macro maps (a map wins
// over a tweak on the same argument), writing only what changed. Unknown
// nodes or arguments are skipped; the return value counts what was
// applied. Message thread (the table's only writer).
int applyPatchTweaks (const Patch& patch, OverrideTable& table);

// Editing a patch's tweak lists (the inspector, message thread). Each keeps
// the lists canonical: one NodeTweak per node, one entry per argument, no
// empty NodeTweak, and an argument is either tweaked or macro-mapped.
// mode 0 removes the argument's tweak.
void setPatchBypass (Patch& patch, const std::string& node, bool on);
void setPatchArgTweak (Patch& patch, const std::string& node, const std::string& arg, int mode, double value);
void setPatchMacroMap (Patch& patch, const std::string& node, const std::string& arg, int macro, double min,
                       double max);
void removePatchMacroMap (Patch& patch, const std::string& node, const std::string& arg);

// "Change this node" (SPEC §9.6, §9.7): gives `into` (the changed patch)
// the tweaks and macro maps of `base` whose node `touched` (the changed
// patch's node list) still has, replacing any of its own. Returns what was
// dropped, one entry per node or map ("flt_lp:1 (cutoff, bypass)",
// "M5 -> flt_lp:1 res"); empty when everything survived. Ids that don't
// parse are dropped too.
std::vector<std::string> carrySurvivingTweaks (const Patch& base, const TouchedNodes& touched, Patch& into);

// Lookups for the UI (null when absent).
const NodeTweak* findNodeTweak (const Patch& patch, const std::string& node);
const ArgTweak* findArgTweak (const Patch& patch, const std::string& node, const std::string& arg);
const MacroMap* findMacroMap (const Patch& patch, const std::string& node, const std::string& arg);

} // namespace vibesynth
