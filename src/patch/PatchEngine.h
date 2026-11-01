#pragma once

// The EEL2 voice/master engine (SPEC §4.2, §4.3, §7.2).
//
// One `EelVoice` owns one EEL2 VM compiled from a patch's `init` + `voice`
// sections (two code handles on the same VM, sharing variables). One
// `EelMaster` owns one VM compiled from `master` with a `MasterRack` and
// (M6, SPEC §5.2) its own `VoiceRack`, so the whole lexicon works there. The audio thread only writes bound variables and calls
// renderSample() / noteOn() / resetState(); all VM creation and
// compilation happens off the audio thread (constructor). Linkage:
// `vibesynth_engine` static lib, shared by the plugin and `vibesynth_cli`
// so the CLI tests exactly the code the plugin runs.
//
// M7: `mw` (mod wheel) is bound in every VM; a voice VM's `srate` is its
// own (oversampled) rate, set by the PatchProgram that builds it.
//
// M5: outputs are raw (no clamp), so the validator (§7.1) can measure NaN
// and peaks above 0 dBFS; the processor's runtime guard (§7.2) does the
// clamping. A non-finite result comes back as a float NaN.
//
// Real-time rules (SPEC §11): renderSample() performs no allocation,
// locks, logging, I/O, exceptions, or std::function. NaN/Inf checks use
// bit-pattern tests, never std::isnan (survives any fast-math flags).

// EEL2 public API first (declares EEL_F/INT_PTR; see VENDORED.md).
#include "eel2/ns-eel.h"
#include "eel2/ns-eel-addfuncs.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include "engine/Lexicon.h"
#include "engine/MasterRack.h"

namespace vibesynth {

// The three code sections of one patch (SPEC §4.2), as the VMs compile
// them. The strings only need to live until the constructor returns.
// Empty (or whitespace/comment-only) init and master are allowed: no init
// code runs, and the master passes inl/inr straight to outl/outr.
struct PatchStrings
{
    const char* init;
    const char* voice;
    const char* master;
};

// Bit-pattern float checks (SPEC §11: survive -ffast-math).
inline bool isFiniteBits (double v) noexcept
{
    uint64_t bits = 0;
    static_assert (sizeof (bits) == sizeof (v), "double must be 64-bit");
    std::memcpy (&bits, &v, sizeof (v));
    return ((bits >> 52) & 0x7FFu) != 0x7FFu;
}

inline bool isFiniteBits (float v) noexcept
{
    uint32_t bits = 0;
    static_assert (sizeof (bits) == sizeof (v), "float must be 32-bit");
    std::memcpy (&bits, &v, sizeof (v));
    return ((bits >> 23) & 0xFFu) != 0xFFu;
}

// A VM output as float, unclamped: non-finite -> NaN (so callers can test
// it), finite values limited to +/-1e6 only so the float cast never turns
// a huge finite double into Inf.
inline float rawOutput (double v) noexcept
{
    if (! isFiniteBits (v))
        return std::numeric_limits<float>::quiet_NaN();
    if (v > 1.0e6)
        return 1.0e6f;
    if (v < -1.0e6)
        return -1.0e6f;
    return static_cast<float> (v);
}

// Host tempo for one sample (SPEC §4.3, M6): `bpm` (120 when the host
// gives none) and `beat`, the position in quarter notes. The default is
// what a VM sees with no host: 120 bpm at beat 0.
struct Transport
{
    static constexpr double kDefaultBpm = 120.0;
    double bpm = kDefaultBpm;
    double beat = 0.0;

    // Beats per sample at this tempo.
    double beatStep (double sampleRate) const noexcept
    {
        return bpm / (60.0 * (sampleRate > 0.0 ? sampleRate : 48000.0));
    }
};

// mem[] slots per VM (SPEC §4.2), preallocated at construction.
constexpr int kVmRamSlots = 65536;

bool isJitUsable();
bool ensureEelInitialised();
const char* eelExecutionMode();

// Macro smoother (SPEC §4.3): one ~10 ms one-pole per macro, stepped once
// per sample and shared by all voices + master.
struct MacroSmoother
{
    static constexpr int kNumMacros = 8;
    double current[kNumMacros] = {};
    double coeff = 1.0;

    void init (double sampleRate) noexcept
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        coeff = 1.0 - std::exp (-1.0 / (0.010 * sr)); // ~10 ms
        for (int i = 0; i < kNumMacros; ++i)
            current[i] = 0.5;
    }

    void step (const float targets[kNumMacros]) noexcept
    {
        for (int i = 0; i < kNumMacros; ++i)
        {
            double t = static_cast<double> (targets[i]);
            if (! isFiniteBits (t)) t = 0.5;
            else if (t < 0.0) t = 0.0;
            else if (t > 1.0) t = 1.0;
            double d = t - current[i];
            if (d > 0.5 || d < -0.5)
                current[i] = t; // snap on huge jumps (patch/default load)
            else
                current[i] += coeff * d;
        }
    }

    void snapAll (const float values[kNumMacros]) noexcept
    {
        for (int i = 0; i < kNumMacros; ++i)
        {
            double v = static_cast<double> (values[i]);
            current[i] = (! isFiniteBits (v) || v < 0.0) ? 0.0
                       : (v > 1.0) ? 1.0 : v;
        }
    }
};

// Voice release tracker (SPEC §4.2 lifetime; the patch owns its envelope
// via env_adsr — C++ only decides when the voice may be freed). M5: owned
// by the processor's voice slot, not the EelVoice, so it survives a patch
// swap (SPEC §7.3).
// A stolen voice gets a 2 ms linear fade (fadeGain() per sample); track()
// returns true once that fade has run out, so the owner frees (or
// retriggers) it.
struct VoiceLifetime
{
    bool gateHeld = false;
    int belowThresholdSamples = 0;
    int releaseSamplesElapsed = 0;
    bool stealFading = false;
    int fadeLength = 1;
    int fadeRemaining = 0;

    static constexpr float kSilenceThreshold = 1e-4f; // -80 dBFS
    static constexpr double kQuietSeconds = 0.05;
    static constexpr double kHardCapSeconds = 10.0;
    static constexpr double kStealFadeSeconds = 0.002;

    void noteOn() noexcept
    {
        gateHeld = true;
        belowThresholdSamples = 0;
        releaseSamplesElapsed = 0;
        stealFading = false;
        fadeRemaining = 0;
    }

    void noteOff() noexcept { gateHeld = false; }

    // Restarting an already-running fade is a no-op (it keeps its ramp).
    void startStealFade (double sampleRate) noexcept
    {
        gateHeld = false;
        if (stealFading)
            return;
        stealFading = true;
        const int n = static_cast<int> (kStealFadeSeconds * sampleRate + 0.5);
        fadeLength = n > 0 ? n : 1;
        fadeRemaining = fadeLength;
    }

    // Per-sample gain: 1 when not fading; during a steal fade a linear ramp
    // (N-1)/N … 0 over fadeLength samples, then 0.
    float fadeGain() noexcept
    {
        if (! stealFading)
            return 1.0f;
        if (fadeRemaining <= 0)
            return 0.0f;
        --fadeRemaining;
        return static_cast<float> (fadeRemaining) / static_cast<float> (fadeLength);
    }

    bool fadeFinished() const noexcept { return stealFading && fadeRemaining <= 0; }

    // True when the voice may be freed: steal fade finished, or released
    // and quiet for 50 ms, or 10 s after release (hard cap).
    bool track (float outSample, double sampleRate) noexcept
    {
        if (stealFading)
            return fadeRemaining <= 0;
        if (gateHeld)
            return false;
        ++releaseSamplesElapsed;
        const float mag = outSample < 0.0f ? -outSample : outSample;
        if (mag < kSilenceThreshold)
            ++belowThresholdSamples;
        else
            belowThresholdSamples = 0;
        if (static_cast<double> (belowThresholdSamples)
                >= kQuietSeconds * sampleRate)
            return true;
        return static_cast<double> (releaseSamplesElapsed)
                >= kHardCapSeconds * sampleRate;
    }
};

// One voice: one EEL2 VM, init + voice code handles, all §4.3 voice vars.
// All creation/compilation in the constructor (off audio thread).
// renderSample()/noteOn()/resetState() are the only audio-thread entry
// points.
class EelVoice
{
public:
    explicit EelVoice (const PatchStrings& patch, double sampleRate,
                       uint32_t voiceSeed);
    ~EelVoice();

    EelVoice (const EelVoice&) = delete;
    EelVoice& operator= (const EelVoice&) = delete;

    bool isValid() const noexcept { return voiceCode_ != nullptr; }
    // On failure: the section that failed ("init", "voice", or empty for a
    // VM setup error) and EEL2's own error text.
    const std::string& compileSection() const noexcept { return compileSection_; }
    const std::string& compileError() const noexcept { return compileError_; }

    VoiceRack& rack() noexcept { return rackStorage_; }
    VmRackContext& rackContext() noexcept { return rackCtx_; }  // M10: the hooked flag

    // `transport` is what init code sees as bpm/beat; `mw` the mod wheel.
    void noteOn (double freq, double note, double vel,
                 const Transport& transport = {}, double mw = 0.0) noexcept;
    float renderSample (double freq, double gate, double note, double vel,
                        const double macros[MacroSmoother::kNumMacros],
                        const Transport& transport = {}, double mw = 0.0) noexcept;

    // Tests only (off the audio thread): a VM variable by name, registered
    // if the code never used it. Null if the voice didn't compile.
    const EEL_F* variable (const char* name) noexcept;

    // Runtime guard (SPEC §7.2): reset the rack, zero every VM variable
    // (pointer list collected at compile time) and mem[]. No allocation.
    void resetState() noexcept;

private:
    NSEEL_VMCTX vm_ = nullptr;
    NSEEL_CODEHANDLE initCode_ = nullptr;
    NSEEL_CODEHANDLE voiceCode_ = nullptr;
    void* gram_ = nullptr;
    VoiceRack rackStorage_;
    VmRackContext rackCtx_;
    EEL_F* freq_ = nullptr;
    EEL_F* note_ = nullptr;
    EEL_F* vel_ = nullptr;
    EEL_F* gate_ = nullptr;
    EEL_F* trig_ = nullptr;
    EEL_F* srate_ = nullptr;
    EEL_F* bpm_ = nullptr;
    EEL_F* beat_ = nullptr;
    EEL_F* macro_[MacroSmoother::kNumMacros] = {};
    EEL_F* mw_ = nullptr;
    EEL_F* out_ = nullptr;
    std::vector<EEL_F_PTR> vars_;
    std::string compileSection_;
    std::string compileError_;
};

// Master bus: one VM, master code handle; its rack context points to both
// a MasterRack (bus_*) and its own VoiceRack (every other function).
class EelMaster
{
public:
    explicit EelMaster (const PatchStrings& patch, double sampleRate);
    ~EelMaster();

    EelMaster (const EelMaster&) = delete;
    EelMaster& operator= (const EelMaster&) = delete;

    // Valid with compiled master code, or as a passthrough (empty master).
    bool isValid() const noexcept { return masterCode_ != nullptr || passthrough_; }
    const std::string& compileError() const noexcept { return compileError_; }

    MasterRack& rack() noexcept { return rackStorage_; }
    VoiceRack& voiceRack() noexcept { return voiceRackStorage_; }
    VmRackContext& rackContext() noexcept { return rackCtx_; }  // M10: the hooked flag

    void renderSample (float inL, float inR, float& outL, float& outR,
                       const double macros[MacroSmoother::kNumMacros],
                       const Transport& transport = {}, double mw = 0.0) noexcept;

    // Resets both racks (patch swap, SPEC §7.3). No allocation.
    void resetRacks() noexcept;

    // Tests only, as EelVoice::variable (null for a passthrough master).
    const EEL_F* variable (const char* name) noexcept;

    // Same as EelVoice::resetState, for the master VM and both racks.
    void resetState() noexcept;

    // Seed for the master's VoiceRack (chaos start points, supersaw phases).
    static constexpr uint32_t kRackSeed = 0x3A57E2u;

private:
    NSEEL_VMCTX vm_ = nullptr;
    NSEEL_CODEHANDLE masterCode_ = nullptr;
    void* gram_ = nullptr;
    MasterRack rackStorage_;
    VoiceRack voiceRackStorage_;
    float* reverbStorage_ = nullptr;
    int reverbCapacity_ = 0;
    float* delayStorage_ = nullptr;  // M12 Tier 2: only when the code calls bus_delay
    VmRackContext rackCtx_;
    EEL_F* inl_ = nullptr;
    EEL_F* inr_ = nullptr;
    EEL_F* outl_ = nullptr;
    EEL_F* outr_ = nullptr;
    EEL_F* srate_ = nullptr;
    EEL_F* bpm_ = nullptr;
    EEL_F* beat_ = nullptr;
    EEL_F* macro_[MacroSmoother::kNumMacros] = {};
    EEL_F* mw_ = nullptr;
    std::vector<EEL_F_PTR> vars_;
    bool passthrough_ = false;
    std::string compileError_;
};

} // namespace vibesynth

