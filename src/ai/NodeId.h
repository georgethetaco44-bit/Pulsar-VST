#pragma once

// M11 (SPEC §9.6, §9.7): node ids for "Change this node".
//
// A node is a (section, module, slot) the offline test saw a patch touch.
// Its id is the graph.edges name with the section in front for master:
//
//   flt_lp:0             voice, slot 0
//   fx_sat#1             voice, stateless shaper, call #1 (the 2nd call)
//   master/bus_sat#1     master, stateless shaper
//   master/flt_lp:0      master, slot 0
//
// parseNodeId also takes the node view's labels ("flt_lp 0", "fx_sat #1"),
// a "voice/" prefix, and ':' or '#' on either kind of module, so the
// inspector and the CLI can pass whatever the user sees. JUCE-free (the
// node view and vibesynth_cli use it).

#include <string>

#include "engine/NodeMeters.h"

namespace vibesynth {

struct NodeRef
{
    bool master = false;
    ModuleId module = ModuleId::Count;
    int slot = 0;

    bool operator== (const NodeRef& o) const noexcept
    {
        return master == o.master && module == o.module && slot == o.slot;
    }
};

// False (with `error` set) for an unknown module, a bad slot or junk.
bool parseNodeId (const std::string& id, NodeRef& out, std::string& error);

// The canonical id ("flt_lp:0", "master/bus_sat#1").
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
