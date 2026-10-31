// M9 node visualizer layout. See NodeLayout.h.

#include "ui/NodeLayout.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <functional>
#include <map>
#include <set>

namespace vibesynth {

namespace {

std::string trim (const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace ((unsigned char) s[a]))
        ++a;
    while (b > a && std::isspace ((unsigned char) s[b - 1]))
        --b;
    return s.substr (a, b - a);
}

// "flt_lp:1" -> (flt_lp, 1); "flt_lp" -> slot 0; false for anything that
// isn't a metered function with a numeric slot.
bool parseNodeName (const std::string& text, ModuleId& module, int& slot)
{
    const auto colon = text.find (':');
    const std::string name = trim (text.substr (0, colon));
    slot = 0;
    if (colon != std::string::npos)
    {
        const std::string num = trim (text.substr (colon + 1));
        if (num.empty() || num.size() > 2)
            return false;
        for (const char c : num)
            if (c < '0' || c > '9')
                return false;
        slot = std::stoi (num);
    }
    return slot < kMaxSlotsPerModule && moduleFromName (name, module);
}

} // namespace

std::string NodeGraphLayout::label (const Node& n)
{
    return std::string (moduleShortName (n.module)) + (isStatelessModule (n.module) ? " #" : " ")
         + std::to_string (n.slot);
}

NodeGraphLayout layoutNodeGraph (const TouchedNodes& touched,
                                 const std::vector<std::pair<std::string, std::string>>& edgeNames)
{
    NodeGraphLayout out;
    auto& nodes = out.nodes;
    for (int region = 0; region < 2; ++region)
        for (int m = 0; m < kNumModules; ++m)
        {
            const uint32_t bits = region == 0 ? touched.voice[m] : touched.master[m];
            for (int s = 0; s < kMaxSlotsPerModule; ++s)
                if ((bits >> s) & 1u)
                {
                    NodeGraphLayout::Node n;
                    n.master = region == 1;
                    n.module = static_cast<ModuleId> (m);
                    n.slot = s;
                    nodes.push_back (n);
                }
        }
    const int numNodes = (int) nodes.size();

    auto findNode = [&nodes] (bool master, ModuleId module, int slot)
    {
        for (int i = 0; i < (int) nodes.size(); ++i)
            if (nodes[(size_t) i].master == master && nodes[(size_t) i].module == module
                && nodes[(size_t) i].slot == slot)
                return i;
        return -1;
    };

    // Graph over the model's names, then contract the ones that aren't nodes.
    std::map<std::string, int> nameId;
    std::vector<int> real;
    std::vector<std::vector<int>> adj;
    auto idOf = [&] (const std::string& raw)
    {
        const std::string name = trim (raw);
        const auto it = nameId.find (name);
        if (it != nameId.end())
            return it->second;
        const int id = (int) real.size();
        nameId.emplace (name, id);
        ModuleId module = ModuleId::Count;
        int slot = 0;
        int node = -1;
        if (parseNodeName (name, module, slot))
        {
            node = findNode (false, module, slot);
            if (node < 0)
                node = findNode (true, module, slot);
        }
        real.push_back (node);
        adj.emplace_back();
        return id;
    };
    for (const auto& e : edgeNames)
    {
        const int a = idOf (e.first);
        const int b = idOf (e.second);
        adj[(size_t) a].push_back (b);
    }

    std::set<std::pair<int, int>> contracted;
    for (int a = 0; a < (int) real.size(); ++a)
    {
        if (real[(size_t) a] < 0)
            continue;
        std::vector<bool> seen (real.size(), false);
        std::deque<int> queue (adj[(size_t) a].begin(), adj[(size_t) a].end());
        while (! queue.empty())
        {
            const int v = queue.front();
            queue.pop_front();
            if (seen[(size_t) v])
                continue;
            seen[(size_t) v] = true;
            if (real[(size_t) v] >= 0)
            {
                if (real[(size_t) v] != real[(size_t) a])
                    contracted.insert ({ real[(size_t) a], real[(size_t) v] });
                continue;
            }
            for (const int w : adj[(size_t) v])
                queue.push_back (w);
        }
    }
    for (const auto& e : contracted)
    {
        NodeGraphLayout::Edge edge;
        edge.from = e.first;
        edge.to = e.second;
        edge.crossRegion = nodes[(size_t) e.first].master != nodes[(size_t) e.second].master;
        out.edges.push_back (edge);
    }

    std::vector<bool> connected ((size_t) numNodes, false);
    for (const auto& e : out.edges)
        connected[(size_t) e.from] = connected[(size_t) e.to] = true;

    for (int region = 0; region < 2; ++region)
    {
        const bool master = region == 1;
        // Edges out of util_safe_fb (the feedback utility) are held back:
        // when one closes a loop it is the loop's feedback edge, so the
        // oscillator it feeds stays at the front.
        std::vector<std::vector<int>> outEdges ((size_t) numNodes);  // edge indices, in-region
        std::vector<int> deferred;
        std::vector<int> inDegree ((size_t) numNodes, 0);
        for (int i = 0; i < (int) out.edges.size(); ++i)
        {
            const auto& e = out.edges[(size_t) i];
            if (e.crossRegion || nodes[(size_t) e.from].master != master)
                continue;
            if (nodes[(size_t) e.from].module == ModuleId::UtilSafeFb)
                deferred.push_back (i);
            else
            {
                outEdges[(size_t) e.from].push_back (i);
                ++inDegree[(size_t) e.to];
            }
        }

        // DFS from the sources first, then the rest, in node order: back
        // edges are feedback.
        std::vector<int> state ((size_t) numNodes, 0);
        std::function<void (int)> dfs = [&] (int v)
        {
            state[(size_t) v] = 1;
            for (const int ei : outEdges[(size_t) v])
            {
                auto& e = out.edges[(size_t) ei];
                if (state[(size_t) e.to] == 1)
                    e.feedback = true;
                else if (state[(size_t) e.to] == 0)
                    dfs (e.to);
            }
            state[(size_t) v] = 2;
        };
        for (int pass = 0; pass < 2; ++pass)
            for (int v = 0; v < numNodes; ++v)
                if (nodes[(size_t) v].master == master && connected[(size_t) v] && state[(size_t) v] == 0
                    && (pass == 1 || inDegree[(size_t) v] == 0))
                    dfs (v);

        auto reaches = [&] (int from, int to)
        {
            std::vector<bool> seen ((size_t) numNodes, false);
            std::vector<int> stack { from };
            while (! stack.empty())
            {
                const int v = stack.back();
                stack.pop_back();
                if (v == to)
                    return true;
                if (seen[(size_t) v])
                    continue;
                seen[(size_t) v] = true;
                for (const int ei : outEdges[(size_t) v])
                    if (! out.edges[(size_t) ei].feedback)
                        stack.push_back (out.edges[(size_t) ei].to);
            }
            return false;
        };
        for (const int ei : deferred)
        {
            auto& e = out.edges[(size_t) ei];
            if (reaches (e.to, e.from))
                e.feedback = true;
            else
                outEdges[(size_t) e.from].push_back (ei);
        }

        // Longest path from the sources over the forward edges.
        std::vector<int> layer ((size_t) numNodes, 0);
        for (int iter = 0; iter < numNodes; ++iter)
        {
            bool changed = false;
            for (int v = 0; v < numNodes; ++v)
                for (const int ei : outEdges[(size_t) v])
                {
                    const auto& e = out.edges[(size_t) ei];
                    if (! e.feedback && layer[(size_t) e.to] < layer[(size_t) v] + 1)
                    {
                        layer[(size_t) e.to] = layer[(size_t) v] + 1;
                        changed = true;
                    }
                }
            if (! changed)
                break;
        }

        int numLayers = 0;
        for (int v = 0; v < numNodes; ++v)
            if (nodes[(size_t) v].master == master && connected[(size_t) v])
                numLayers = std::max (numLayers, layer[(size_t) v] + 1);
        std::vector<std::vector<int>> byLayer ((size_t) numLayers);
        for (int v = 0; v < numNodes; ++v)
            if (nodes[(size_t) v].master == master && connected[(size_t) v])
                byLayer[(size_t) layer[(size_t) v]].push_back (v);

        std::vector<double> rowOf ((size_t) numNodes, 0.0);
        for (int l = 0; l < numLayers; ++l)
        {
            auto& members = byLayer[(size_t) l];
            std::vector<double> key ((size_t) numNodes, 1.0e9);
            for (const int v : members)
            {
                double sum = 0.0;
                int count = 0;
                for (const auto& e : out.edges)
                    if (e.to == v && ! e.feedback && ! e.crossRegion && layer[(size_t) e.from] < l)
                    {
                        sum += rowOf[(size_t) e.from];
                        ++count;
                    }
                if (count > 0)
                    key[(size_t) v] = sum / count;
            }
            std::sort (members.begin(), members.end(), [&key] (int a, int b)
                       {
                           if (key[(size_t) a] < key[(size_t) b]) return true;
                           if (key[(size_t) b] < key[(size_t) a]) return false;
                           return a < b;
                       });
            for (int r = 0; r < (int) members.size(); ++r)
            {
                rowOf[(size_t) members[(size_t) r]] = r;
                nodes[(size_t) members[(size_t) r]].layer = l;
                nodes[(size_t) members[(size_t) r]].row = r;
            }
            out.rows[region] = std::max (out.rows[region], (int) members.size());
        }
        out.layers[region] = numLayers;

        int looseCount = 0;
        for (int v = 0; v < numNodes; ++v)
            if (nodes[(size_t) v].master == master && ! connected[(size_t) v])
            {
                nodes[(size_t) v].loose = true;
                nodes[(size_t) v].row = looseCount++;
            }
        out.loose[region] = looseCount;
    }
    return out;
}

} // namespace vibesynth
