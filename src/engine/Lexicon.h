// M4 — Lexicon.h: module IDs, VoiceRack + client-owned storage notes,
// registration declaration.
//
// SPEC §5.1/§5.2. Slot pools are PER FUNCTION NAME (amendment 1): e.g.
// osc_sine(0,…) and osc_saw(0,…) are independent oscillators.
// Every stateful call records |out| peak and a touched bit (relaxed
// atomics); stateless shapers record by call index (recordStateless).
// Wrappers never allocate. All state is pre-allocated in
// VoiceRack::init() / MasterRack::init() from prepareToPlay.
//
// Include order: DaisySP headers first (they pull <random>/<vector> via
// Utility/dsp.h); our own headers after they close their namespaces.
// Nothing in this header may open `namespace vibesynth {` before the
// DaisySP includes.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

// DaisySP (MIT repo, pinned SHA; daisysp_mit target).
#include "Synthesis/oscillator.h"
#include "Control/adsr.h"
#include "Effects/decimator.h"
#include "Effects/chorus.h"
#include "PhysicalModeling/resonator.h"
#include "PhysicalModeling/KarplusString.h"

// Own DSP (after DaisySP includes; see note at the top).
#include "engine/ChaosMath.h"
#include "engine/Filters.h"
#include "engine/Utilities.h"
#include "engine/Utilities2.h"
#include "engine/Utilities3.h"
#include "engine/Utilities4.h"
#include "engine/Utilities5.h"

namespace vibesynth {

// ---------------------------------------------------------------------------
// Per-function module IDs (amendment 1: one pool per lexicon function).
// kMaxSlotsPerModule covers the largest pool (4).

enum class ModuleId : int
{
    OscSine = 0,
    OscSaw,
    OscTri,
    OscSquare,
    OscSupersaw,
    FltLp,
    FltHp,
    FltBp,
    FltLadder,
    EnvAdsr,
    EnvFollow,
    FxFold,      // stateless (fx_fold, fx_sat, util_bytebeat, bus_sat): the
    FxSat,       // nth call in a sample meters as slot n (recordStateless)
    FxCrush,
    FxComb,
    FxOtt,
    PhysResonator,
    PhysString,
    ChaosLorenz,
    ChaosDuffing,
    UtilSafeFb,
    UtilXover,
    UtilBytebeat,
    BusSat,      // master-bus functions (meter slot 0; stereo ones record
    BusMonoLow,  // max(|L|, |R|))
    BusChorus,
    BusReverb,
    Count
};

constexpr int kNumModules = static_cast<int> (ModuleId::Count);
constexpr int kMaxSlotsPerModule = 4;

// SPEC §5.2 MVP slot counts (per function), incl. amendment-1 duplication.
constexpr int kNumOscSlots = 4;       // sine/saw/tri/square each
constexpr int kNumSupersawSlots = 2;
constexpr int kNumSvfSlots = 3;       // lp/hp/bp each
constexpr int kNumLadderSlots = 2;
constexpr int kNumAdsrSlots = 3;
constexpr int kNumEnvFollowSlots = 2;
constexpr int kNumCrushSlots = 2;
constexpr int kNumCombSlots = 2;
constexpr int kNumOttSlots = 2;
constexpr int kNumResonatorSlots = 1;
constexpr int kNumStringSlots = 1;
constexpr int kNumLorenzSlots = 2;    // count unstated in SPEC; chosen
constexpr int kNumDuffingSlots = 2;
constexpr int kNumSafeFbSlots = 4;    // count unstated in SPEC; chosen
constexpr int kNumXoverSlots = 2;

// ---------------------------------------------------------------------------
// Setter caches (SPEC §4.2.1 wrapper rules). Mappings are cached on their
// RAW inputs (before any pow), and coefficient-heavy Daisy setters run at
// control rate: once every kControlInterval samples per slot (`countdown`
// starts at 0, so the first call is a tick). A default-constructed cache
// forces every setter to run on the next tick (used by VoiceRack::reset).

constexpr int kControlInterval = 16;

struct OscCache     { float hz = -1.0f; float pw = -1.0f; };

// flt_* (M7): own TPT SVF and ZDF ladder (engine/Filters.h) smooth and map
// their inputs per sample, so they need no control-rate cache.

// ADSR: times/sustain update at ticks and on every gate edge (so a new
// note always starts with its own times); the gate itself is per sample.
struct AdsrCache    { double rawA = -1.0; double rawD = -1.0;
                      double rawS = -1.0; double rawR = -1.0;
                      bool gate = false; int countdown = 0; };
struct CrushCache   { float bits = -1.0f; float rate = -1.0f; };
struct ResonCache   { float hz = -1.0f; float st = -1.0f;
                      float br = -1.0f; float da = -1.0f; int countdown = 0; };
struct StringCache  { float hz = -1.0f; float br = -1.0f; float da = -1.0f;
                      int countdown = 0; };
// Chaos caches hold the rate/hz-derived step sizes (pow/cos/sin are too
// costly to recompute per sample).
struct LorenzCache  { float rate = -1.0f; double dt = 0.0; int steps = 1; };
struct DuffingCache { float hz = -1.0f; double rotCos = 1.0; double rotSin = 0.0;
                      double h = 0.0; int steps = 1; double dcR = 0.0; };

struct OverrideTable;  // engine/NodeOverrides.h (M10)
struct TraceSink;

// ---------------------------------------------------------------------------
// VoiceRack: all module state for one voice. init() pre-allocates (off the
// audio thread); reset() clears state without allocating; recordMeter()
// is the only audio-thread write to meters/touched (relaxed atomics).

struct VoiceRack
{
    double sampleRate = 48000.0;

    daisysp::Oscillator oscSine[kNumOscSlots];
    daisysp::Oscillator oscSaw[kNumOscSlots];
    daisysp::Oscillator oscTri[kNumOscSlots];
    daisysp::Oscillator oscSquare[kNumOscSlots];
    OscCache oscSineCache[kNumOscSlots];
    OscCache oscSawCache[kNumOscSlots];
    OscCache oscTriCache[kNumOscSlots];
    OscCache oscSquareCache[kNumOscSlots];

    SupersawState supersaw[kNumSupersawSlots];

    FilterRate filterRate;
    SvfState fltLp[kNumSvfSlots];
    SvfState fltHp[kNumSvfSlots];
    SvfState fltBp[kNumSvfSlots];
    LadderState fltLadder[kNumLadderSlots];

    daisysp::Adsr envAdsr[kNumAdsrSlots];
    AdsrCache adsrCache[kNumAdsrSlots];

    EnvFollowState envFollow[kNumEnvFollowSlots];

    daisysp::Decimator crush[kNumCrushSlots];
    CrushCache crushCache[kNumCrushSlots];

    CombState comb[kNumCombSlots];
    // 10 Hz at a 192 kHz voice rate (96 kHz host at 2x, M7); see init().
    static constexpr int kCombCapacity = 19200;
    float combBuf[kNumCombSlots][kCombCapacity];

    OttState ott[kNumOttSlots];

    daisysp::Resonator resonator[kNumResonatorSlots];
    ResonCache resonCache[kNumResonatorSlots];
    daisysp::String string_[kNumStringSlots];
    StringCache stringCache[kNumStringSlots];

    LorenzState lorenz[kNumLorenzSlots];
    LorenzCache lorenzCache[kNumLorenzSlots];
    DuffingState duffing[kNumDuffingSlots];
    DuffingCache duffingCache[kNumDuffingSlots];

    SafeFbState safeFb[kNumSafeFbSlots];
    XoverState xover[kNumXoverSlots];

    NoiseState noise;

    std::atomic<float> meters[kNumModules][kMaxSlotsPerModule];
    std::atomic<uint32_t> touched[kNumModules];
    uint8_t statelessCalls[kNumModules] = {};

    // M10 hooks (SPEC §9.6; engine/LexiconHooks.h). PatchPlayer sets them
    // per chunk from the processor's OverrideTable and TraceBank, never
    // through a program; reset() leaves them alone. (The fast-path flag is
    // VmRackContext::hooked.) `rackKind` is RackKind (0 voice, 1 master:
    // the master VM's own VoiceRack). `macroVars` are the VM's m1..m8, set
    // when the VM is built, for macro maps.
    uint8_t rackKind = 0;
    const OverrideTable* overrides = nullptr;
    TraceSink* capture = nullptr;
    const double* macroVars[8] = {};


    void init (double newSampleRate, uint32_t voiceSeed);
    void reset() noexcept;
    void recordMeter (ModuleId module, int slot, float out) noexcept;
    void beginSample() noexcept { std::memset (statelessCalls, 0, sizeof (statelessCalls)); }
    void recordStateless (ModuleId module, float out) noexcept;
};

} // namespace vibesynth
