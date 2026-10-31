#pragma once

// M9 node visualizer layout (SPEC §9.6). JUCE-free (vibesynth_cli tests
// it), computed once per node list and edge list, never per frame, so the
// picture doesn't move while the sound plays.
//
// Nodes are the touched (module, slot) pairs, voice first, then master,
// each in ModuleId order. Stateless shapers (fx_sat, fx_fold, ...) are
// nodes too, their slot being the call index in a sample ("fx_sat #1").
// Edges are the patch's graph.edges ("flt_lp:0" names; no ":slot" means
// slot 0, so "fx_sat" is fx_sat #0) between real nodes. A name that is not
// a node (out, noise, the model's own variables) is contracted: A -> x ->
// B becomes A -> B, so the descriptive flow survives. A name resolves to
// the voice node if the voice touched it, else to the master node.
//
// Layout per region: nodes with at least one edge are layered by longest
// path from the sources over the region's own edges, after a DFS in node
// order marks back edges (one-sample feedback loops) so cycles are
// allowed; rows inside a layer follow the mean row of the predecessors
// (ties in node order). Nodes without any edge go in a separate row.

#include <string>
#include <utility>
#include <vector>

#include "engine/NodeMeters.h"

namespace vibesynth {

struct NodeGraphLayout
{
    struct Node
    {
        bool master = false;
        ModuleId module = ModuleId::Count;
        int slot = 0;
        int layer = 0;            // column inside the region (connected nodes)
        int row = 0;              // row inside the layer, or index in the loose row
        bool loose = false;       // no edges: drawn in the region's loose row
    };
    struct Edge
    {
        int from = 0;             // indices into nodes
        int to = 0;
        bool feedback = false;    // a back edge (part of a cycle)
        bool crossRegion = false; // voice -> master
    };

    std::vector<Node> nodes;
    std::vector<Edge> edges;
    int layers[2] = {};    // [voice, master]: number of layers of connected nodes
    int rows[2] = {};      // the tallest layer
    int loose[2] = {};     // nodes in the loose row

    // "flt_lp 0", or "fx_sat #0" for a stateless shaper
    static std::string label (const Node& n);
};

NodeGraphLayout layoutNodeGraph (const TouchedNodes& touched,
                                 const std::vector<std::pair<std::string, std::string>>& edgeNames);

} // namespace vibesynth
