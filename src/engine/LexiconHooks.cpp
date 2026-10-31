// M10: the override / bypass / trace hook layer. See LexiconHooks.h.

#include "engine/LexiconArgs.h"
#include "engine/LexiconHooks.h"
#include "engine/NodeOverrides.h"

namespace vibesynth {

namespace {

// Inline copies of NodeMeters' isStatelessModule (this is the hot path).
inline bool statelessModule (int m) noexcept
{
    return m == static_cast<int> (ModuleId::FxFold) || m == static_cast<int> (ModuleId::FxSat)
        || m == static_cast<int> (ModuleId::UtilBytebeat) || m == static_cast<int> (ModuleId::BusSat);
}

bool isMasterRackModule (int m) noexcept
{
    return m == static_cast<int> (ModuleId::BusMonoLow) || m == static_cast<int> (ModuleId::BusChorus)
        || m == static_cast<int> (ModuleId::BusReverb);
}

} // namespace

EEL_F callHooked (const LexiconEntry& e, LexiconWrapperFn fn, void* ctx, INT_PTR np, EEL_F** p) noexcept
{
    auto* rc = static_cast<VmRackContext*> (ctx);
    VoiceRack* vr = rc->voice;
    const int m = e.module;
    if (m < 0 || m >= kNumModules || np < e.numArgs || e.numArgs > kMaxLexiconArgs
        || (e.scope == LexiconScope::Master && rc->master == nullptr))
        return fn (ctx, np, p);

    for (int i = 0; i < e.numArgs; ++i)
        if (p[i] == nullptr)
            return fn (ctx, np, p);

    int slot = 0;
    const bool stateless = statelessModule (m);
    if (e.numSlots > 0)
        slot = clampSlot (static_cast<double> (*p[0]), e.numSlots);
    else if (stateless)
        slot = vr->statelessCalls[m];  // this call's index (recordStateless bumps it)

    const NodeOverride* node = vr->overrides != nullptr ? vr->overrides->activeNode (vr->rackKind, m, slot) : nullptr;
    TraceSink* sink = e.primary ? vr->capture : nullptr;
    if (node == nullptr && sink == nullptr)
        return fn (ctx, np, p);

    EEL_F vals[kMaxLexiconArgs];
    EEL_F* ptrs[kMaxLexiconArgs];
    for (int i = 0; i < e.numArgs; ++i)
    {
        vals[i] = *p[i];
        ptrs[i] = &vals[i];
    }

    bool bypass = false;
    if (node != nullptr)
    {
        bypass = e.bypass != BypassKind::None && node->bypass.load (std::memory_order_relaxed) != 0;
        if (e.primary)
            for (int i = 0; i < e.numArgs; ++i)
            {
                const auto& ov = node->args[i];
                const auto mode = static_cast<ArgMode> (ov.mode.load (std::memory_order_acquire));
                if (mode == ArgMode::None || ! isEditableArg (e.args[i].kind))
                    continue;
                double macro = 0.0;
                if (mode == ArgMode::Macro)
                {
                    const int mi = ov.macro.load (std::memory_order_relaxed) & 7;
                    const double* mv = vr->macroVars[mi];
                    macro = mv != nullptr ? *mv : 0.0;
                }
                vals[i] = static_cast<EEL_F> (overrideArg (e.args[i], mode, ov.applied.load (std::memory_order_relaxed),
                                                           ov.mapLo.load (std::memory_order_relaxed),
                                                           ov.mapHi.load (std::memory_order_relaxed), macro,
                                                           static_cast<double> (vals[i])));
            }
    }

    EEL_F result = 0.0;
    if (bypass && (e.bypass == BypassKind::PassArg || e.bypass == BypassKind::Zero))
    {
        result = e.bypass == BypassKind::PassArg && e.bypassArg >= 0 && e.bypassArg < e.numArgs ? vals[e.bypassArg]
                                                                                                 : 0.0;
        // Meter as the wrapper would, so the call numbering and the touched
        // set don't change.
        const float metered = sanitizeFloat (static_cast<float> (result));
        if (stateless)
            vr->recordStateless (static_cast<ModuleId> (m), metered);
        else if (isMasterRackModule (m))
            rc->master->recordMeter (static_cast<ModuleId> (m), 0, metered);
        else
            vr->recordMeter (static_cast<ModuleId> (m), slot, metered);
    }
    else
    {
        if (bypass && e.bypass == BypassKind::ForceArg && e.bypassArg >= 0 && e.bypassArg < e.numArgs)
            vals[e.bypassArg] = static_cast<EEL_F> (e.bypassValue);
        result = fn (ctx, static_cast<INT_PTR> (e.numArgs), ptrs);
    }

    if (sink != nullptr)
        sink->record (e, m, slot, static_cast<float> (result), vals);  // EEL_F is double (PatchEngine.cpp)
    return result;
}

} // namespace vibesynth
