#pragma once

// M10: node overrides and node traces (SPEC §9.6, §11). JUCE-free, part of
// vibesynth_engine.
//
//   OverrideTable  what the inspector set: per (rack, module, slot or
//                  stateless call index, argument) a mode (offset /
//                  replace / macro map) and its values, plus a bypass flag
//                  per node. Processor-owned and fixed-size; the message
//                  thread is the only writer, the audio thread reads it
//                  (relaxed/acquire atomics, no locks). PatchPlayer points
//                  the racks at it each chunk, never through a program.
//   TraceBank      what the inspector and the node pills draw: per node a
//                  decimated trace of its output and the live (effective)
//                  value of each editable argument. Written only for the
//                  newest sounding voice (TraceBank::voice) and the master
//                  (TraceBank::master), read by the UI at 30 fps. Audio-rate
//                  nodes publish whole frames (about two cycles of the
//                  voice's frequency, triggered on a rising zero crossing)
//                  through a two-buffer sequence lock; control-rate nodes
//                  (env_*, slow chaos) roll a ~2 s ring.
//
// Override maths (SPEC §9.6), applied before the native function runs:
//   offset   normalized kinds: x + k * (hi - lo), clamped to [lo, hi];
//            hz: x * 2^(st / 12) (the wrapper clamps Hz itself)
//   replace  the value (clamped to [lo, hi] when set)
//   macro    lo + m * (hi - lo), m the VM's own mN (what the code reads)
// overrideArg() is the one implementation, shared with --bake's literals
// (offsetApplied), so a baked patch computes bit-identical arguments.

#include <atomic>
#include <cstdint>

#include "engine/Lexicon.h"
#include "engine/LexiconTable.h"

namespace vibesynth {

enum class RackKind : uint8_t { Voice = 0, Master = 1 };
constexpr int kNumRackKinds = 2;

enum class ArgMode : uint8_t { None = 0, Offset = 1, Replace = 2, Macro = 3 };

// The double the engine adds (normalized kinds) or multiplies by (hz) for
// an Offset of `user` (normalized units, or semitones for hz).
double offsetApplied (const LexiconArg& arg, double user) noexcept;
// The value a Replace of `user` uses (clamped to the argument's range).
double replaceApplied (const LexiconArg& arg, double user) noexcept;
// The effective argument: `x` (the code's value) under the override.
// `applied` is offsetApplied() for Offset, the value for Replace; `macro`
// is the macro's value (Macro mode only). Built without FP contraction.
double overrideArg (const LexiconArg& arg, ArgMode mode, double applied, double mapLo, double mapHi,
                    double macro, double x) noexcept;

struct ArgOverride
{
    std::atomic<uint8_t> mode { 0 };      // ArgMode
    std::atomic<uint8_t> macro { 0 };     // 0..7 (m1..m8)
    std::atomic<double> applied { 0.0 };  // see overrideArg
    std::atomic<double> mapLo { 0.0 };
    std::atomic<double> mapHi { 1.0 };
};

struct NodeOverride
{
    std::atomic<uint8_t> bypass { 0 };
    ArgOverride args[kMaxLexiconArgs];
};

struct OverrideTable
{
    NodeOverride nodes[kNumRackKinds][kNumModules][kMaxSlotsPerModule];
    std::atomic<uint32_t> active[kNumRackKinds][kNumModules];  // bit s: node (m, s) has something set
    std::atomic<uint32_t> anyActive[kNumRackKinds];            // count of active nodes

    OverrideTable() noexcept;
    OverrideTable (const OverrideTable&) = delete;
    OverrideTable& operator= (const OverrideTable&) = delete;

    // Message thread (the only writer).
    void clear() noexcept;
    void setBypass (RackKind rack, ModuleId module, int slot, bool on) noexcept;
    // `user` is the persisted value (offset amount / replace value); for
    // Macro, `lo`/`hi` are the map range and `macro` 0..7.
    void setArg (RackKind rack, ModuleId module, int slot, int arg, ArgMode mode, double user, int macro = 0,
                 double lo = 0.0, double hi = 1.0) noexcept;
    void clearArg (RackKind rack, ModuleId module, int slot, int arg) noexcept;

    // Audio thread: the node's overrides, or null when nothing is set.
    const NodeOverride* activeNode (int rack, int module, int slot) const noexcept
    {
        if (((active[rack][module].load (std::memory_order_acquire) >> slot) & 1u) == 0u)
            return nullptr;
        return &nodes[rack][module][slot];
    }
    bool anyFor (RackKind rack) const noexcept
    {
        return anyActive[(int) rack].load (std::memory_order_relaxed) != 0u;
    }

private:
    void refresh (int rack, int module, int slot) noexcept;
};

// ---------------------------------------------------------------------------
// Traces.

struct NodeTrace
{
    static constexpr int kPoints = 256;    // per frame / ring
    enum Mode : uint8_t { None = 0, Frame = 1, Ring = 2 };

    // Reader-visible.
    std::atomic<uint8_t> mode { None };
    std::atomic<uint32_t> frameSeq { 0 };      // completed frames; frame[(seq) & 1] is the newest
    std::atomic<uint16_t> frameLen[2] = {};
    std::atomic<float> frame[2][kPoints];
    std::atomic<uint32_t> ringPos { 0 };       // points written so far
    std::atomic<float> ring[kPoints];
    std::atomic<float> args[kMaxLexiconArgs];  // live effective values
    std::atomic<uint32_t> lastBlock { 0 };     // TraceSink::block at the last write

    // Audio-thread-only capture state.
    float prev = 0.0f;
    bool capturing = false;
    int armedFor = 0;
    int filled = 0;
    int length = 0;
    double step = 1.0;
    double acc = 0.0;
    int decim = 0;
    int argDecim = 0;

    NodeTrace() noexcept;
};

struct TraceSink
{
    double rate = 96000.0;   // samples per second of the racks feeding it
    double freq = 110.0;     // the newest voice's frequency (frame length)
    double cycle = 872.7;    // rate / freq, samples per cycle
    int ringEvery = 750;     // samples per rolling-trace point (~2 s per ring)
    uint32_t block = 0;      // bumped by the player each chunk
    NodeTrace nodes[kNumModules][kMaxSlotsPerModule];

    // Audio thread, once per chunk (PatchPlayer).
    void setTiming (double newRate, double newFreq) noexcept
    {
        rate = newRate > 0.0 ? newRate : 48000.0;
        freq = newFreq < 20.0 ? 20.0 : (newFreq > 20000.0 ? 20000.0 : newFreq);
        cycle = rate / freq;
        const int every = static_cast<int> (rate * 2.0 / NodeTrace::kPoints);
        ringEvery = every > 1 ? every : 1;
    }

    // Audio thread (the hook layer): one call's output and effective args.
    void record (const LexiconEntry& e, int module, int slot, float out, const double* args) noexcept;
};

struct TraceBank
{
    TraceSink voice;
    TraceSink master;
    std::atomic<bool> enabled { false };  // an editor wants traces (no capture cost otherwise)

    TraceSink& sink (RackKind k) noexcept { return k == RackKind::Master ? master : voice; }
};

// UI: copies a node's newest trace into out[0..n) (n <= kPoints, oldest
// first). `ring` true for a rolling control trace. False when the node has
// produced nothing yet. A frame that is overwritten mid-copy is retried.
bool readNodeTrace (const NodeTrace& t, float* out, int& n, bool& ring) noexcept;

} // namespace vibesynth
