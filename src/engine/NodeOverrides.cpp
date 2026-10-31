// M10: node overrides and traces. See NodeOverrides.h.
//
// Built with -ffp-contract=off (CMakeLists.txt): overrideArg() must round
// exactly like the separate multiply and add EEL2's JIT emits for --bake's
// "lo + mN * d", or a chaotic patch would drift away from its baked twin.

#include "engine/NodeOverrides.h"

#include <cmath>

namespace vibesynth {

double offsetApplied (const LexiconArg& arg, double user) noexcept
{
    if (! isFiniteBitsD (user))
        return arg.kind == ArgKind::Hz ? 1.0 : 0.0;
    if (arg.kind == ArgKind::Hz)
        return std::exp2 (user / 12.0);
    return user * (arg.hi - arg.lo);
}

double replaceApplied (const LexiconArg& arg, double user) noexcept
{
    if (! isFiniteBitsD (user))
        return arg.lo;
    return user < arg.lo ? arg.lo : (user > arg.hi ? arg.hi : user);
}

double overrideArg (const LexiconArg& arg, ArgMode mode, double applied, double mapLo, double mapHi, double macro,
                    double x) noexcept
{
    switch (mode)
    {
        case ArgMode::Offset:
        {
            if (arg.kind == ArgKind::Hz)
                return x * applied;
            double v = x + applied;
            v = v < arg.lo ? arg.lo : v;
            v = v > arg.hi ? arg.hi : v;
            return v;
        }
        case ArgMode::Replace:
            return applied;
        case ArgMode::Macro:
        {
            const double span = mapHi - mapLo;
            const double scaled = macro * span;
            return mapLo + scaled;
        }
        case ArgMode::None:
            break;
    }
    return x;
}

//==============================================================================
OverrideTable::OverrideTable() noexcept
{
    clear();
}

void OverrideTable::clear() noexcept
{
    for (int r = 0; r < kNumRackKinds; ++r)
    {
        for (int m = 0; m < kNumModules; ++m)
        {
            active[r][m].store (0u, std::memory_order_release);
            for (int s = 0; s < kMaxSlotsPerModule; ++s)
            {
                auto& n = nodes[r][m][s];
                n.bypass.store (0, std::memory_order_relaxed);
                for (auto& a : n.args)
                {
                    a.mode.store (0, std::memory_order_relaxed);
                    a.macro.store (0, std::memory_order_relaxed);
                    a.applied.store (0.0, std::memory_order_relaxed);
                    a.mapLo.store (0.0, std::memory_order_relaxed);
                    a.mapHi.store (1.0, std::memory_order_relaxed);
                }
            }
        }
        anyActive[r].store (0u, std::memory_order_release);
    }
}

void OverrideTable::refresh (int r, int m, int s) noexcept
{
    const auto& n = nodes[r][m][s];
    bool on = n.bypass.load (std::memory_order_relaxed) != 0;
    for (const auto& a : n.args)
        on = on || a.mode.load (std::memory_order_relaxed) != 0;
    const uint32_t bit = 1u << s;
    const uint32_t was = active[r][m].load (std::memory_order_relaxed);
    const uint32_t now = on ? (was | bit) : (was & ~bit);
    if (now == was)
        return;
    active[r][m].store (now, std::memory_order_release);
    const uint32_t count = anyActive[r].load (std::memory_order_relaxed);
    anyActive[r].store (on ? count + 1u : (count > 0u ? count - 1u : 0u), std::memory_order_release);
}

void OverrideTable::setBypass (RackKind rack, ModuleId module, int slot, bool on) noexcept
{
    const int r = (int) rack, m = (int) module;
    if (m < 0 || m >= kNumModules || slot < 0 || slot >= kMaxSlotsPerModule)
        return;
    nodes[r][m][slot].bypass.store (on ? 1 : 0, std::memory_order_release);
    refresh (r, m, slot);
}

void OverrideTable::setArg (RackKind rack, ModuleId module, int slot, int arg, ArgMode mode, double user, int macro,
                            double lo, double hi) noexcept
{
    const int r = (int) rack, m = (int) module;
    if (m < 0 || m >= kNumModules || slot < 0 || slot >= kMaxSlotsPerModule || arg < 0 || arg >= kMaxLexiconArgs)
        return;
    const LexiconEntry* e = primaryLexiconEntry (m);
    if (e == nullptr || arg >= e->numArgs || ! isEditableArg (e->args[arg].kind))
        return;
    const LexiconArg& meta = e->args[arg];
    auto& a = nodes[r][m][slot].args[arg];
    // A mode change goes through None, so the audio thread never pairs the
    // new values with the old mode.
    if (a.mode.load (std::memory_order_relaxed) != (uint8_t) mode)
        a.mode.store (0, std::memory_order_release);
    double applied = 0.0;
    if (mode == ArgMode::Offset)
        applied = offsetApplied (meta, user);
    else if (mode == ArgMode::Replace)
        applied = replaceApplied (meta, user);
    a.applied.store (applied, std::memory_order_relaxed);
    a.mapLo.store (isFiniteBitsD (lo) ? lo : meta.lo, std::memory_order_relaxed);
    a.mapHi.store (isFiniteBitsD (hi) ? hi : meta.hi, std::memory_order_relaxed);
    a.macro.store ((uint8_t) (macro & 7), std::memory_order_relaxed);
    a.mode.store ((uint8_t) mode, std::memory_order_release);
    refresh (r, m, slot);
}

void OverrideTable::clearArg (RackKind rack, ModuleId module, int slot, int arg) noexcept
{
    const int r = (int) rack, m = (int) module;
    if (m < 0 || m >= kNumModules || slot < 0 || slot >= kMaxSlotsPerModule || arg < 0 || arg >= kMaxLexiconArgs)
        return;
    nodes[r][m][slot].args[arg].mode.store (0, std::memory_order_release);
    refresh (r, m, slot);
}

bool lexiconTraceIsControl (const LexiconEntry& e, const double* args) noexcept
{
    switch (e.trace)
    {
        case TraceKind::Control: return true;
        case TraceKind::ByRate:
        {
            const int k = e.traceArg;
            if (k < 0 || k >= e.numArgs || args == nullptr)
                return false;
            const double x = args[k];
            if (! isFiniteBitsD (x))
                return false;
            // Below ~20 Hz the node is a modulator: a rolling trace.
            // chaos_lorenz's rate maps 0.05 Hz * 20000^rate (LexiconFn4.cpp),
            // which is 20 Hz at rate = ln 400 / ln 20000 = 0.605.
            if (e.args[k].kind == ArgKind::Norm)
                return x < 0.605;
            return x < 20.0;
        }
        case TraceKind::None:
        case TraceKind::Audio:
            break;
    }
    return false;
}

//==============================================================================
NodeTrace::NodeTrace() noexcept
{
    for (int b = 0; b < 2; ++b)
    {
        frameLen[b].store (0, std::memory_order_relaxed);
        for (auto& v : frame[b])
            v.store (0.0f, std::memory_order_relaxed);
    }
    for (auto& v : ring)
        v.store (0.0f, std::memory_order_relaxed);
    for (auto& v : args)
        v.store (0.0f, std::memory_order_relaxed);
}

void TraceSink::record (const LexiconEntry& e, int m, int slot, float out, const double* args) noexcept
{
    if (m < 0 || m >= kNumModules || slot < 0 || slot >= kMaxSlotsPerModule || e.trace == TraceKind::None)
        return;
    NodeTrace& t = nodes[m][slot];
    // Live argument values: ~1.5 kHz is plenty for a 30 fps UI.
    if (--t.argDecim <= 0)
    {
        t.argDecim = 64;
        for (int i = 0; i < e.numArgs && i < kMaxLexiconArgs; ++i)
            t.args[i].store (static_cast<float> (args[i]), std::memory_order_relaxed);
    }
    if (! isFiniteBitsD (static_cast<double> (out)))
        out = 0.0f;

    if (lexiconTraceIsControl (e, args))
    {
        // Rolling ~2 s: one point every rate * 2 / kPoints samples.
        if (t.mode.load (std::memory_order_relaxed) != NodeTrace::Ring)
        {
            t.mode.store (NodeTrace::Ring, std::memory_order_release);
            t.decim = 0;
        }
        if (--t.decim <= 0)
        {
            t.decim = ringEvery;
            const uint32_t pos = t.ringPos.load (std::memory_order_relaxed);
            t.ring[pos % NodeTrace::kPoints].store (out, std::memory_order_relaxed);
            t.ringPos.store (pos + 1u, std::memory_order_release);
        }
    }
    else
    {
        if (t.mode.load (std::memory_order_relaxed) != NodeTrace::Frame)
        {
            t.mode.store (NodeTrace::Frame, std::memory_order_release);
            t.capturing = false;
            t.armedFor = 0;
        }
        if (! t.capturing)
        {
            // Wait for a rising zero crossing (at most two cycles, so DC and
            // noise still show), then take about two cycles.
            ++t.armedFor;
            if ((t.prev < 0.0f && out >= 0.0f) || t.armedFor > static_cast<int> (cycle * 2.0) + 2)
            {
                const double total = 2.0 * cycle;
                t.capturing = true;
                t.filled = 0;
                if (total <= NodeTrace::kPoints)
                {
                    t.step = 1.0;
                    t.length = total < 8.0 ? 8 : static_cast<int> (total);
                }
                else
                {
                    t.step = total / NodeTrace::kPoints;
                    t.length = NodeTrace::kPoints;
                }
                t.acc = t.step;  // the crossing sample is the first point
            }
        }
        if (t.capturing)
        {
            t.acc += 1.0;
            if (t.acc >= t.step + 1.0)
            {
                t.acc -= t.step;
                const uint32_t seq = t.frameSeq.load (std::memory_order_relaxed);
                const int back = static_cast<int> ((seq + 1u) & 1u);
                t.frame[back][t.filled].store (out, std::memory_order_relaxed);
                if (++t.filled >= t.length)
                {
                    t.frameLen[back].store (static_cast<uint16_t> (t.length), std::memory_order_relaxed);
                    t.frameSeq.store (seq + 1u, std::memory_order_release);
                    t.capturing = false;
                    t.armedFor = 0;
                }
            }
        }
    }
    t.prev = out;
    if (t.lastBlock.load (std::memory_order_relaxed) != block)
        t.lastBlock.store (block, std::memory_order_relaxed);
}

bool readNodeTrace (const NodeTrace& t, float* out, int& n, bool& ring) noexcept
{
    n = 0;
    const auto mode = t.mode.load (std::memory_order_acquire);
    if (mode == NodeTrace::Ring)
    {
        ring = true;
        const uint32_t pos = t.ringPos.load (std::memory_order_acquire);
        if (pos == 0u)
            return false;
        const uint32_t count = pos < (uint32_t) NodeTrace::kPoints ? pos : (uint32_t) NodeTrace::kPoints;
        const uint32_t start = pos - count;
        for (uint32_t i = 0; i < count; ++i)
            out[i] = t.ring[(start + i) % NodeTrace::kPoints].load (std::memory_order_relaxed);
        n = (int) count;
        return true;
    }
    if (mode != NodeTrace::Frame)
        return false;
    ring = false;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const uint32_t seq = t.frameSeq.load (std::memory_order_acquire);
        if (seq == 0u)
            return false;
        const int b = (int) (seq & 1u);
        int len = t.frameLen[b].load (std::memory_order_relaxed);
        len = len > NodeTrace::kPoints ? NodeTrace::kPoints : len;
        for (int i = 0; i < len; ++i)
            out[i] = t.frame[b][i].load (std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_acquire);
        if (t.frameSeq.load (std::memory_order_relaxed) == seq)
        {
            n = len;
            return len > 0;
        }
    }
    return false;
}

} // namespace vibesynth
