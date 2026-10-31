#pragma once

// Node ids (SPEC §9.6, §9.7): the one implementation, shared by the node
// view, the inspector, saved tweaks (patch/PatchTweaks.h), bake, "Change
// this node" and vibesynth_cli. JUCE-free, part of vibesynth_engine.
//
// A node is a (rack, module, slot) the offline test saw a patch touch. Its
// id is the graph.edges name with the region in front for master:
//
//   flt_lp:0             voice, slot 0
//   fx_sat#1             voice, stateless shaper, call #1 (the 2nd call)
//   master/bus_sat#1     master, stateless shaper
//   master/flt_lp:0      master, slot 0
//   master/bus_reverb    bus effects without a slot (bus_mono_low,
//                        bus_chorus, bus_reverb) have no slot suffix
//
// The module name is NodeMeters' moduleShortName (util_xover covers
// util_xover_lo/_hi). parseNodeId also takes the node view's labels
// ("flt_lp 0", "fx_sat #1"), any case, surrounding spaces, a "voice/"
// prefix, a bare name (slot 0), the lexicon aliases moduleFromName accepts,
// and ':' or '#' on either kind of module, so the inspector, saved files
// and the CLI can pass whatever the user sees.

#include <string>

#include "engine/NodeMeters.h"
#include "engine/NodeOverrides.h"

namespace vibesynth {

struct NodeRef
{
    RackKind rack = RackKind::Voice;
    ModuleId module = ModuleId::Count;
    int slot = 0;

    bool master() const noexcept { return rack == RackKind::Master; }
    bool operator== (const NodeRef& o) const noexcept
    {
        return rack == o.rack && module == o.module && slot == o.slot;
    }
    bool operator!= (const NodeRef& o) const noexcept { return ! (*this == o); }
};

// False (with `error` set) for an unknown module, a bad slot or junk.
bool parseNodeId (const std::string& id, NodeRef& out, std::string& error);
inline bool parseNodeId (const std::string& id, NodeRef& out)
{
    std::string error;
    return parseNodeId (id, out, error);
}

// The canonical id ("flt_lp:0", "master/bus_sat#1", "master/bus_reverb").
// parseNodeId reads it back to the same node.
std::string nodeIdString (const NodeRef& node);

// One line for the model: which call in which section the id means.
std::string describeNodeForPrompt (const NodeRef& node);

// True if the patch's touched set has the node.
bool nodeIsTouched (const TouchedNodes& touched, const NodeRef& node) noexcept;

// Nodes of `before` that `after` no longer has, as canonical ids joined by
// ", " (empty when every node survived). "Change this node" keeps the
// tweaks whose node survived (SPEC §9.6) and reports the rest.
std::string lostNodes (const TouchedNodes& before, const TouchedNodes& after);

} // namespace vibesynth
