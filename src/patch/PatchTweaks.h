#pragma once

// M10 (SPEC §9.6): node ids and the bridge between a Patch's saved tweaks
// and the processor's OverrideTable. JUCE-free, part of vibesynth_engine.
//
// Node id format (the graph.edges spelling, plus the region):
//   voice nodes   "flt_lp:0"         module name, ':' and the slot
//                 "fx_sat#1"         stateless shapers: '#' and the call index
//   master nodes  "master/flt_lp:0", "master/bus_sat#1",
//                 "master/bus_reverb" (bus_* without a slot)
// The module name is NodeMeters' moduleShortName (util_xover covers
// util_xover_lo/_hi). parseNodeId also accepts a bare name (slot 0) and the
// lexicon aliases moduleFromName accepts.

#include <string>

#include "engine/NodeOverrides.h"
#include "patch/Patch.h"

namespace vibesynth {

struct NodeRef
{
    RackKind rack = RackKind::Voice;
    ModuleId module = ModuleId::Count;
    int slot = 0;

    bool operator== (const NodeRef& o) const noexcept
    {
        return rack == o.rack && module == o.module && slot == o.slot;
    }
};

std::string nodeId (const NodeRef& n);
bool parseNodeId (const std::string& id, NodeRef& out);

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

// Lookups for the UI (null when absent).
const NodeTweak* findNodeTweak (const Patch& patch, const std::string& node);
const ArgTweak* findArgTweak (const Patch& patch, const std::string& node, const std::string& arg);
const MacroMap* findMacroMap (const Patch& patch, const std::string& node, const std::string& arg);

} // namespace vibesynth
