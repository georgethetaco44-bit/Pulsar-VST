// M4 — MasterRack.h: MasterRack, VmRackContext + registration declaration.
#pragma once

#include <atomic>
#include <cstdint>

#include "engine/Lexicon.h"

namespace vibesynth {

// ---------------------------------------------------------------------------
// MasterRack: bus module state (M4 runs it at 1×; same struct works for any
// rate passed to init). Own FDN reverb (DaisySP ReverbSc is LGPL — SPEC §5.2
// forbids it) + own LR4 mono-low + 2× DaisySP ChorusEngine for stereo in/out,
// and (M12 Tier 2) the ping-pong delay and the width stage (Utilities6.h).

// Phase 2: 8 delay lines (Hadamard feedback), two input allpasses per
// channel, slow modulation of the line lengths, decay time set by `size`
// and the wet level normalised to the input's (see LexiconFn6.cpp).
struct FdnReverbState
{
    static constexpr int kNumLines = 8;
    static constexpr int kNumDiffusers = 4;  // L: 0, 1; R: 2, 3
    // Lengths @48 kHz (primes), scaled by sr/48000. Fixed: `size` sets the
    // decay time only, so moving it can't click or bend the pitch.
    static constexpr int kBaseLen[kNumLines] = { 977, 1103, 1279, 1427, 1601, 1787, 1949, 2111 };
    static constexpr int kDiffLen[kNumDiffusers] = { 241, 409, 263, 431 };
    // Line length swing, +-1 ms. It averages the response to steady tones
    // over time: at 1/6 of this, single saw pitches scattered further from
    // the input level (measured, phase 2).
    static constexpr double kModDepth48k = 48.0;
    static constexpr int kMargin = 4;             // interpolation margin

    float* buf[kNumLines] = {};
    int capacity[kNumLines] = {};
    int writePos[kNumLines] = {};
    float baseLen[kNumLines] = {};   // at this rate, samples
    float lpState[kNumLines] = {};   // per-line damping one-poles
    float dcIn[kNumLines] = {};      // per-line DC blockers: with long decays the loop's
    float dcOut[kNumLines] = {};     // gain near 0 Hz would otherwise lift any DC far above the wet level
    float* diffBuf[kNumDiffusers] = {};
    int diffLen[kNumDiffusers] = {};
    int diffPos[kNumDiffusers] = {};

    // Derived from size and the rate. Recomputed when size changes, but at
    // most every kSizeInterval samples (8 pow and a sqrt), so a moving size
    // costs about 10 % more than a still one; the 5 ms glide covers the steps.
    static constexpr int kSizeInterval = 16;
    double cachedSize = -1.0;
    int sizeHold = 0;                  // samples until size may be read again
    float gainTarget[kNumLines] = {};  // per-line loop gain for the decay time
    float wetScaleTarget = 0.0f;       // wet level normalisation
    float gain[kNumLines] = {};        // the same, gliding (about 5 ms)
    float wetScale = 0.0f;
    float glide = 0.004f;              // one-pole coefficient, 5 ms
    bool primed = false;               // false: the next call snaps to the targets
    float damp = 0.5f;               // damping one-pole coefficient
    float dcPole = 0.999f;           // DC blocker pole (10 Hz)
    float modDepth = 0.0f;           // samples at this rate
    // Two slow quadrature LFOs (rotating phasors) for the line modulation.
    double rotC[2] = {}, rotS[2] = {};
    double lfoC[2] = { 1.0, 1.0 }, lfoS[2] = {};
    // Mix gains, recomputed only when mix changes, then gliding.
    double cachedMix = -1.0;
    float dryTarget = 1.0f, wetTarget = 0.0f;
    float dryGain = 1.0f, wetGain = 0.0f;
    bool mixPrimed = false;

    // Floats of storage init() needs at this rate (EelMaster allocates it).
    static int storageFor (double sampleRate) noexcept;
    void init (double sampleRate, float* storage, int capacityFloats) noexcept;
    void reset() noexcept;
    // One stereo step; returns the wet signal (before the mix).
    void process (float inL, float inR, double size, double sampleRate, float& wetL, float& wetR) noexcept;
    // Decay time (s) for size 0-1: 0.3 * 20^size.
    static double rt60For (double size) noexcept;
};

struct MasterRack
{
    double sampleRate = 48000.0;

    daisysp::ChorusEngine chorusL;
    daisysp::ChorusEngine chorusR;
    float chorusMix = 0.5f;

    XoverState monoXoverL;
    XoverState monoXoverR;
    float monoRightHi = 0.0f; // cached by bus_mono_low for bus_mono_low_r()

    FdnReverbState reverb;
    float* reverbStorage = nullptr; // owned by EelMaster (FdnReverbState::storageFor)
    int reverbCapacity = 0;
    float reverbRight = 0.0f; // cached by bus_reverb for bus_reverb_r()
    float chorusRight = 0.0f; // cached by bus_chorus for bus_chorus_r()

    // M12 Tier 2. The delay buffer is owned by EelMaster (allocated off the
    // audio thread, 2 x DelayBusState::framesFor(sr) floats, only when the
    // master code calls bus_delay); init() carves it up. `bpm` is the
    // host tempo, set per sample by EelMaster::renderSample.
    DelayBusState delay;
    float* delayStorage = nullptr;
    int delayFrames = 0;
    float delayRight = 0.0f;  // cached by bus_delay for bus_delay_r()
    WidthBusState width;
    float widthRight = 0.0f;  // cached by bus_width for bus_width_r()
    MbCompState mb;           // phase 2: bus_mb
    float mbRight = 0.0f;     // cached by bus_mb for bus_mb_r()
    double bpm = 120.0;

    std::atomic<float> meters[kNumModules][kMaxSlotsPerModule];
    std::atomic<uint32_t> touched[kNumModules];

    void init (double newSampleRate);
    void reset() noexcept;
    void recordMeter (ModuleId module, int slot, float out) noexcept;
};

// ---------------------------------------------------------------------------
// Per-VM rack context. A voice VM has only `voice`; the master VM has both
// (M6, SPEC §5.2: its own VoiceRack runs osc_*/flt_*/... with their own
// slots). bus_* wrappers return 0 on a voice VM instead of misinterpreting
// the other rack type.

struct VmRackContext
{
    VoiceRack* voice = nullptr;
    MasterRack* master = nullptr;
    void* vm = nullptr;
    // M10: true while the VM's racks have overrides to apply or a trace to
    // write (PatchPlayer, per chunk). False keeps every lexicon call on the
    // unchanged fast path: the thunk's one load and one branch.
    bool hooked = false;
    // M13: while hooked, bit m = send module m's calls through the hook
    // layer (it has a node set, or this VM records traces: all bits). Set
    // with `hooked` (PatchPlayer, per segment); a node set in between waits
    // for the next segment. All bits = every call, as in M10.
    uint64_t hookModules = ~uint64_t { 0 };
};

// Registers every SPEC §5.2 MVP native function (once, off the audio
// thread, from the EEL init path in PatchEngine.cpp). Wrappers reach the
// calling VM's rack through NSEEL_VM_SetCustomFuncThis (SPEC §4.2).
void registerLexiconFunctions();

// M5 validator note (amendment 4): a cheap per-(function, slot) call counter
// plus a per-VM sample counter would let the offline validator reject a
// patch that calls the same slot twice in one sample. NOT implemented here.

} // namespace vibesynth
