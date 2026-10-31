// M10: saved tweaks. See PatchTweaks.h.

#include "patch/PatchTweaks.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "engine/NodeMeters.h"

namespace vibesynth {

int applyPatchTweaks (const Patch& patch, OverrideTable& table, bool includeMods)
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
        double modDepth[kNumModSources] = {};
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
        w.mode = ArgMode::Macro;
        w.user = 0.0;
        w.macro = m.macro - 1;
        w.lo = m.min;
        w.hi = m.max;
        ++applied;
    }

    double macroModWant[8][kNumModSources] = {};
    if (includeMods)
    {
        for (int i = 0; i < kNumModLfos; ++i)
            table.setLfoSpec (i, patch.mods.lfos[(size_t) i]);
        for (int i = 0; i < kNumModEnvs; ++i)
            table.setEnvSpec (i, patch.mods.envs[(size_t) i]);

        for (const auto& a : patch.mods.assignments)
        {
            const int src = static_cast<int> (a.source);
            if (src < 0 || src >= kNumModSources || std::abs (a.depth) < 1.0e-6)
                continue;
            if (a.isMacroTarget())
            {
                macroModWant[a.macro - 1][src] = std::clamp (a.depth, -1.0, 1.0);
                ++applied;
            }
            else if (a.isNodeTarget())
            {
                NodeRef n;
                if (! parseNodeId (a.node, n))
                    continue;
                const LexiconEntry* e = primaryLexiconEntry ((int) n.module);
                const int k = e != nullptr ? lexiconArgIndex (*e, a.arg.c_str()) : -1;
                if (k < 0 || ! isEditableArg (e->args[k].kind))
                    continue;
                at (n).args[k].modDepth[src] = std::clamp (a.depth, -1.0, 1.0);
                ++applied;
            }
        }
    }
    else
    {
        for (int i = 0; i < kNumModLfos; ++i)
            table.setLfoSpec (i, ModLfoSpec {});
        for (int i = 0; i < kNumModEnvs; ++i)
            table.setEnvSpec (i, ModEnvSpec {});
    }

    for (int m = 0; m < 8; ++m)
        for (int src = 0; src < kNumModSources; ++src)
            table.setMacroMod (m, static_cast<ModSource> (src), macroModWant[m][src]);

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
                    for (int src = 0; src < kNumModSources; ++src)
                    {
                        const double cur = node.args[k].modDepth[src].load (std::memory_order_relaxed);
                        if (! samePatchNumber (cur, a.modDepth[src])
                            || ((node.args[k].modMask.load (std::memory_order_relaxed) & (1u << src)) != 0u)
                                   != (std::abs (a.modDepth[src]) >= 1.0e-6))
                            table.setArgMod (n.rack, n.module, s, k, static_cast<ModSource> (src), a.modDepth[src]);
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

void setPatchModLfo (Patch& p, int index, const ModLfoSpec& spec)
{
    if (index < 0 || index >= kNumModLfos)
        return;
    ModLfoSpec clean = spec;
    clean.rateHz = quantisePatchNumber (std::clamp (isFiniteBitsD (spec.rateHz) ? spec.rateHz : 2.0, 0.01, 50.0));
    p.mods.lfos[(size_t) index] = clean;
}

void setPatchModEnv (Patch& p, int index, const ModEnvSpec& spec)
{
    if (index < 0 || index >= kNumModEnvs)
        return;
    ModEnvSpec clean;
    clean.attack  = quantisePatchNumber (std::clamp (isFiniteBitsD (spec.attack)  ? spec.attack  : 0.01, 0.001, 10.0));
    clean.decay   = quantisePatchNumber (std::clamp (isFiniteBitsD (spec.decay)   ? spec.decay   : 0.2,  0.001, 10.0));
    clean.sustain = quantisePatchNumber (std::clamp (isFiniteBitsD (spec.sustain) ? spec.sustain : 0.7,  0.0,   1.0));
    clean.release = quantisePatchNumber (std::clamp (isFiniteBitsD (spec.release) ? spec.release : 0.3,  0.001, 10.0));
    p.mods.envs[(size_t) index] = clean;
}

bool setPatchMacroMod (Patch& p, ModSource source, int macro, double depth)
{
    if (macro < 1 || macro > Patch::kMaxMacros)
        return false;
    const double q = quantisePatchNumber (std::clamp (isFiniteBitsD (depth) ? depth : 0.0, -1.0, 1.0));
    if (std::abs (q) < 1.0e-6)
    {
        removePatchMacroMod (p, macro, static_cast<int> (source));
        return true;
    }
    for (auto& a : p.mods.assignments)
    {
        if (a.isMacroTarget() && a.macro == macro)
        {
            a.source = source;
            a.depth = q;
            return true;
        }
    }
    if ((int) p.mods.assignments.size() >= kMaxModAssignments)
        return false;
    ModAssignment ma;
    ma.source = source;
    ma.macro = macro;
    ma.depth = q;
    p.mods.assignments.push_back (ma);
    return true;
}

void removePatchMacroMod (Patch& p, int macro, int sourceFilter)
{
    p.mods.assignments.erase (
        std::remove_if (p.mods.assignments.begin(), p.mods.assignments.end(),
                        [&] (const ModAssignment& a)
                        {
                            return a.isMacroTarget() && a.macro == macro
                                && (sourceFilter < 0 || static_cast<int> (a.source) == sourceFilter);
                        }),
        p.mods.assignments.end());
}

bool setPatchArgMod (Patch& p, ModSource source, const std::string& node, const std::string& arg, double depth)
{
    if (node.empty() || arg.empty())
        return false;
    const double q = quantisePatchNumber (std::clamp (isFiniteBitsD (depth) ? depth : 0.0, -1.0, 1.0));
    if (std::abs (q) < 1.0e-6)
    {
        removePatchArgMod (p, node, arg, static_cast<int> (source));
        return true;
    }
    for (auto& a : p.mods.assignments)
    {
        if (a.isNodeTarget() && a.node == node && a.arg == arg)
        {
            a.source = source;
            a.depth = q;
            return true;
        }
    }
    if ((int) p.mods.assignments.size() >= kMaxModAssignments)
        return false;
    ModAssignment ma;
    ma.source = source;
    ma.node = node;
    ma.arg = arg;
    ma.depth = q;
    p.mods.assignments.push_back (ma);
    return true;
}

void removePatchArgMod (Patch& p, const std::string& node, const std::string& arg, int sourceFilter)
{
    p.mods.assignments.erase (
        std::remove_if (p.mods.assignments.begin(), p.mods.assignments.end(),
                        [&] (const ModAssignment& a)
                        {
                            return a.isNodeTarget() && a.node == node && a.arg == arg
                                && (sourceFilter < 0 || static_cast<int> (a.source) == sourceFilter);
                        }),
        p.mods.assignments.end());
}

std::vector<std::string> carrySurvivingTweaks (const Patch& base, const TouchedNodes& touched, Patch& into)
{
    std::vector<std::string> dropped;
    auto survives = [&touched] (const std::string& id)
    {
        NodeRef n;
        return parseNodeId (id, n) && nodeIsTouched (touched, n);
    };
    into.tweaks.clear();
    into.macroMaps.clear();
    for (const auto& t : base.tweaks)
    {
        if (survives (t.node))
        {
            into.tweaks.push_back (t);
            continue;
        }
        std::string what;
        for (const auto& a : t.args)
            what += (what.empty() ? "" : ", ") + a.arg;
        if (t.bypass)
            what += what.empty() ? "bypass" : ", bypass";
        dropped.push_back (t.node + (what.empty() ? "" : " (" + what + ")"));
    }
    for (const auto& m : base.macroMaps)
    {
        if (survives (m.node))
            into.macroMaps.push_back (m);
        else
            dropped.push_back ("M" + std::to_string (m.macro) + " -> " + m.node + " " + m.arg);
    }

    // M13: Change this node keeps modulation on macros, and on knobs whose
    // node survives. Drops the rest and names them in the status detail.
    into.mods.lfos = base.mods.lfos;
    into.mods.envs = base.mods.envs;
    into.mods.assignments.clear();
    for (const auto& a : base.mods.assignments)
    {
        if (a.isMacroTarget() || (a.isNodeTarget() && survives (a.node)))
            into.mods.assignments.push_back (a);
        else if (a.isNodeTarget())
            dropped.push_back (std::string (modSourceLabel (a.source)) + " -> " + a.node + " " + a.arg);
    }
    return dropped;
}

void carryMutateMods (const Patch& base, Patch& into)
{
    into.mods.lfos = base.mods.lfos;
    into.mods.envs = base.mods.envs;
    into.mods.assignments.clear();
    for (const auto& a : base.mods.assignments)
        if (a.isMacroTarget())
            into.mods.assignments.push_back (a);
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

const ModAssignment* findMacroMod (const Patch& p, int macro)
{
    for (const auto& a : p.mods.assignments)
        if (a.isMacroTarget() && a.macro == macro)
            return &a;
    return nullptr;
}

const ModAssignment* findArgMod (const Patch& p, const std::string& node, const std::string& arg)
{
    for (const auto& a : p.mods.assignments)
        if (a.isNodeTarget() && a.node == node && a.arg == arg)
            return &a;
    return nullptr;
}

} // namespace vibesynth
