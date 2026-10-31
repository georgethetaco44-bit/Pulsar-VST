// M4 — MasterRack.h: MasterRack, VmRackContext + registration declaration.
#pragma once

#include <atomic>
#include <cstdint>

#include "engine/Lexicon.h"

namespace vibesynth {

// ---------------------------------------------------------------------------
// MasterRack: bus module state (M4 runs it at 1×; same struct works for any
// rate passed to init). Own FDN reverb (DaisySP ReverbSc is LGPL — SPEC §5.2
// forbids it) + own LR4 mono-low + 2× DaisySP ChorusEngine for stereo in/out.

struct FdnReverbState
{
    static constexpr int kNumLines = 4;
    // Base delay lengths @48 kHz; scaled by sr/48000 × (0.75 + 0.5·size).
    // Storage is sized for the 1.5× worst case (see EelMaster ctor).
    static constexpr int kBaseLen[kNumLines] = { 1429, 1601, 1787, 1999 };
    static constexpr int kMaxExtra = 64; // interp margin
    float* buf[kNumLines] = {};
    int capacity[kNumLines] = {};
    int writePos[kNumLines] = {};
    float delayLen[kNumLines] = {}; // current fractional lengths, samples
    float fb = 0.6f;                // loop feedback
    float lpState[kNumLines] = {};  // per-line damping one-poles

    void reset() noexcept;
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
    float* reverbStorage = nullptr; // owned, sized in init()
    int reverbCapacity = 0;
    float reverbRight = 0.0f; // cached by bus_reverb for bus_reverb_r()
    float chorusRight = 0.0f; // cached by bus_chorus for bus_chorus_r()

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
