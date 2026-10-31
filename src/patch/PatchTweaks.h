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

// Makes `table` hold exactly the patch's tweaks, macro maps and (when
// `includeMods` is true) modulator specs and assignments, writing only what
// changed. Unknown nodes or arguments are skipped; the return value counts
// what was applied. Message thread (the table's only writer).
int applyPatchTweaks (const Patch& patch, OverrideTable& table, bool includeMods = true);

// Editing a patch's tweak lists (the inspector, message thread). Each keeps
// the lists canonical: one NodeTweak per node, one entry per argument, no
// empty NodeTweak, and an argument is either tweaked or macro-mapped.
// mode 0 removes the argument's tweak.
void setPatchBypass (Patch& patch, const std::string& node, bool on);
void setPatchArgTweak (Patch& patch, const std::string& node, const std::string& arg, int mode, double value);
void setPatchMacroMap (Patch& patch, const std::string& node, const std::string& arg, int macro, double min,
                       double max);
void removePatchMacroMap (Patch& patch, const std::string& node, const std::string& arg);

// M13 (SPEC §9.8): editing a patch's modulator specs and assignments
// (capped at kMaxModAssignments = 8). Setting depth == 0 removes that
// assignment. Returns false if adding a new assignment would exceed 8.
void setPatchModLfo (Patch& patch, int index, const ModLfoSpec& spec);
void setPatchModEnv (Patch& patch, int index, const ModEnvSpec& spec);
bool setPatchMacroMod (Patch& patch, ModSource source, int macro, double depth);
void removePatchMacroMod (Patch& patch, int macro, int sourceFilter = -1);
bool setPatchArgMod (Patch& patch, ModSource source, const std::string& node, const std::string& arg, double depth);
void removePatchArgMod (Patch& patch, const std::string& node, const std::string& arg, int sourceFilter = -1);

// "Change this node" (SPEC §9.6, §9.7, §9.8): gives `into` (the changed
// patch) the tweaks, macro maps and mod assignments of `base` whose node
// `touched` (the changed patch's node list) still has, plus all macro
// modulations, replacing any of its own. Returns what was dropped, one
// entry per node, map or mod ("flt_lp:1 (cutoff, bypass)",
// "M5 -> flt_lp:1 res", "LFO 1 -> flt_lp:1 cutoff"); empty when everything
// survived.
std::vector<std::string> carrySurvivingTweaks (const Patch& base, const TouchedNodes& touched, Patch& into);

// M13 Mutate rule: keeps macro modulation and drops modulation on node knobs.
void carryMutateMods (const Patch& base, Patch& into);

// Lookups for the UI (null when absent).
const NodeTweak* findNodeTweak (const Patch& patch, const std::string& node);
const ArgTweak* findArgTweak (const Patch& patch, const std::string& node, const std::string& arg);
const MacroMap* findMacroMap (const Patch& patch, const std::string& node, const std::string& arg);
const ModAssignment* findMacroMod (const Patch& patch, int macro);
const ModAssignment* findArgMod (const Patch& patch, const std::string& node, const std::string& arg);

} // namespace vibesynth
