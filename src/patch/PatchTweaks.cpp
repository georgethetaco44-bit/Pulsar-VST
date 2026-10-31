// M10: node ids and saved tweaks. See PatchTweaks.h.

#include "patch/PatchTweaks.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "engine/NodeMeters.h"

namespace vibesynth {

namespace {

bool isSlotless (ModuleId m)
{
    return m == ModuleId::BusMonoLow || m == ModuleId::BusChorus || m == ModuleId::BusReverb;
}

bool parseInt (const std::string& s, int& out)
{
    if (s.empty() || s.size() > 3)
        return false;
    for (const char c : s)
        if (c < '0' || c > '9')
            return false;
    out = std::atoi (s.c_str());
    return true;
}

} // namespace

std::string nodeId (const NodeRef& n)
{
    std::string id = n.rack == RackKind::Master ? "master/" : "";
    id += moduleShortName (n.module);
    if (isStatelessModule (n.module))
        id += "#" + std::to_string (n.slot);
    else if (! isSlotless (n.module))
        id += ":" + std::to_string (n.slot);
    return id;
}

bool parseNodeId (const std::string& idIn, NodeRef& out)
{
    std::string id = idIn;
    NodeRef r;
    if (id.rfind ("master/", 0) == 0)
    {
        r.rack = RackKind::Master;
        id = id.substr (7);
    }
    const size_t sep = id.find_first_of (":#");
    const std::string name = id.substr (0, sep);
    if (! moduleFromName (name, r.module))
        return false;
    r.slot = 0;
    if (sep != std::string::npos && ! parseInt (id.substr (sep + 1), r.slot))
        return false;
    if (r.slot < 0 || r.slot >= kMaxSlotsPerModule)
        return false;
    out = r;
    return true;
}

int applyPatchTweaks (const Patch& patch, OverrideTable& table)
{
    // What the table should hold, then only the differences are written,
    // so a knob drag never leaves the audio thread a moment with nothing
    // applied.
    struct Want
    {
        ArgMode mode = ArgMode::None;
        double user = 0.0;
        int macro = 0;
        double lo = 0.0, hi = 1.0;
    };
    struct NodeWant
    {
        bool bypass = false;
        Want args[kMaxLexiconArgs];
    };
    std::vector<NodeWant> want ((size_t) (kNumRackKinds * kNumModules * kMaxSlotsPerModule));
    auto at = [&want] (const NodeRef& n) -> NodeWant&
    {
        return want[(size_t) (((int) n.rack * kNumModules + (int) n.module) * kMaxSlotsPerModule + n.slot)];
    };

    int applied = 0;
    for (const auto& t : patch.tweaks)
    {
        NodeRef n;
        if (! parseNodeId (t.node, n))
            continue;
        const LexiconEntry* e = primaryLexiconEntry ((int) n.module);
        if (e == nullptr)
            continue;
        if (t.bypass && e->bypass != BypassKind::None)
        {
            at (n).bypass = true;
            ++applied;
        }
        for (const auto& a : t.args)
        {
            const int k = lexiconArgIndex (*e, a.arg.c_str());
            if (k < 0 || ! isEditableArg (e->args[k].kind))
                continue;
            auto& w = at (n).args[k];
            w.mode = a.mode == ArgTweak::kReplace ? ArgMode::Replace : ArgMode::Offset;
            w.user = a.value;
            ++applied;
        }
    }
    for (const auto& m : patch.macroMaps)
    {
        NodeRef n;
        if (! parseNodeId (m.node, n) || m.macro < 1 || m.macro > 8)
            continue;
        const LexiconEntry* e = primaryLexiconEntry ((int) n.module);
        const int k = e != nullptr ? lexiconArgIndex (*e, m.arg.c_str()) : -1;
        if (k < 0 || ! isEditableArg (e->args[k].kind))
            continue;
        auto& w = at (n).args[k];
        w = { ArgMode::Macro, 0.0, m.macro - 1, m.min, m.max };
        ++applied;
    }

    for (int r = 0; r < kNumRackKinds; ++r)
        for (int m = 0; m < kNumModules; ++m)
            for (int s = 0; s < kMaxSlotsPerModule; ++s)
            {
                const NodeRef n { (RackKind) r, (ModuleId) m, s };
                const NodeWant& w = at (n);
                auto& node = table.nodes[r][m][s];
                if ((node.bypass.load() != 0) != w.bypass)
                    table.setBypass (n.rack, n.module, s, w.bypass);
                for (int k = 0; k < kMaxLexiconArgs; ++k)
                {
                    const Want& a = w.args[k];
                    if (a.mode == ArgMode::None)
                    {
                        if (node.args[k].mode.load() != 0)
                            table.clearArg (n.rack, n.module, s, k);
                    }
                    else
                    {
                        table.setArg (n.rack, n.module, s, k, a.mode, a.user, a.macro, a.lo, a.hi);
                    }
                }
            }
    return applied;
}

//==============================================================================
namespace {

NodeTweak& nodeTweakFor (Patch& p, const std::string& node)
{
    for (auto& t : p.tweaks)
        if (t.node == node)
            return t;
    p.tweaks.push_back ({});
    p.tweaks.back().node = node;
    return p.tweaks.back();
}

void dropEmpty (Patch& p)
{
    p.tweaks.erase (std::remove_if (p.tweaks.begin(), p.tweaks.end(),
                                    [] (const NodeTweak& t) { return ! t.bypass && t.args.empty(); }),
                    p.tweaks.end());
}

void removeArgTweak (Patch& p, const std::string& node, const std::string& arg)
{
    for (auto& t : p.tweaks)
        if (t.node == node)
            t.args.erase (std::remove_if (t.args.begin(), t.args.end(),
                                          [&] (const ArgTweak& a) { return a.arg == arg; }),
                          t.args.end());
    dropEmpty (p);
}

} // namespace

void setPatchBypass (Patch& p, const std::string& node, bool on)
{
    if (on)
        nodeTweakFor (p, node).bypass = true;
    else
        for (auto& t : p.tweaks)
            if (t.node == node)
                t.bypass = false;
    dropEmpty (p);
}

void setPatchArgTweak (Patch& p, const std::string& node, const std::string& arg, int mode, double value)
{
    if (mode != ArgTweak::kOffset && mode != ArgTweak::kReplace)
    {
        removeArgTweak (p, node, arg);
        return;
    }
    removePatchMacroMap (p, node, arg);
    auto& t = nodeTweakFor (p, node);
    for (auto& a : t.args)
        if (a.arg == arg)
        {
            a.mode = mode;
            a.value = quantisePatchNumber (value);
            return;
        }
    t.args.push_back ({ arg, mode, quantisePatchNumber (value) });
}

void setPatchMacroMap (Patch& p, const std::string& node, const std::string& arg, int macro, double min, double max)
{
    removeArgTweak (p, node, arg);
    for (auto& m : p.macroMaps)
        if (m.node == node && m.arg == arg)
        {
            m.macro = macro;
            m.min = quantisePatchNumber (min);
            m.max = quantisePatchNumber (max);
            return;
        }
    MacroMap m;
    m.macro = macro;
    m.node = node;
    m.arg = arg;
    m.min = quantisePatchNumber (min);
    m.max = quantisePatchNumber (max);
    p.macroMaps.push_back (m);
}

void removePatchMacroMap (Patch& p, const std::string& node, const std::string& arg)
{
    p.macroMaps.erase (std::remove_if (p.macroMaps.begin(), p.macroMaps.end(),
                                       [&] (const MacroMap& m) { return m.node == node && m.arg == arg; }),
                       p.macroMaps.end());
}

const NodeTweak* findNodeTweak (const Patch& p, const std::string& node)
{
    for (const auto& t : p.tweaks)
        if (t.node == node)
            return &t;
    return nullptr;
}

const ArgTweak* findArgTweak (const Patch& p, const std::string& node, const std::string& arg)
{
    if (const auto* t = findNodeTweak (p, node))
        for (const auto& a : t->args)
            if (a.arg == arg)
                return &a;
    return nullptr;
}

const MacroMap* findMacroMap (const Patch& p, const std::string& node, const std::string& arg)
{
    for (const auto& m : p.macroMaps)
        if (m.node == node && m.arg == arg)
            return &m;
    return nullptr;
}

} // namespace vibesynth
