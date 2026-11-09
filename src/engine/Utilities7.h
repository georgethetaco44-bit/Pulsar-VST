// Phase 2 — Utilities.h part 7: fx_comb_ff (feedback + feedforward comb) and
// bus_mb (3-band upward/downward compressor), own DSP. Method bodies and the
// lexicon wrappers live in LexiconFn8.cpp.
//
// Real-time rules (SPEC 8.4): no allocation, locks, logging, I/O or
// exceptions on the audio thread; NaN/Inf checks use bit-pattern tests.
#pragma once

#include <cstdint>
#include <cstring>

#include "engine/Utilities.h"

namespace vibesynth {

// ---------------------------------------------------------------------------
// fx_comb_ff(slot, in, hz, feedback, feedforward):
//   H(z) = (1 + ff * Z) / (1 - fb * Z), Z a delay of 1/hz seconds.
// Direct form II on one delay line: w = in + fb * w(t - D); out = w + ff * w(t - D).
// The delay is fractional (Lagrange, centred, odd order so its magnitude
// never exceeds 1: 3rd order at voice rates, 5th at the master's 48 kHz) and follows hz through two one-poles of 2.5 ms
// in series, so a moving hz glides instead of clicking (with one one-pole the
// delay's speed jumps, which put energy 35 dB below the signal above 8 kHz on
// a 440 -> 73.2 Hz jump, measured). The first call after a reset starts at
// the target delay.

constexpr int kNumCombFfSlots = 2;

struct CombFfState
{
    static constexpr double kMinHz = 10.0;
    static constexpr double kSmoothSeconds = 0.0025;  // each of the two stages
    static constexpr double kMinDelay = 3.0;   // the taps reach 2 samples short of the delay

    float* buf = nullptr;      // owned by the VoiceRack (allocated only when the code uses it)
    int capacity = 0;          // samples
    int writePos = 0;
    double delay = 0.0;        // smoothed, samples
    double delay1 = 0.0;       // the first smoothing stage
    double target = kMinDelay; // samples, for cachedHz
    double cachedHz = -1.0;
    double weightsFor = -1.0;  // the delay `weights` were computed for
    float weights[6] = {};
    double smooth = 0.0;       // one-pole coefficient at smoothRate
    double smoothRate = -1.0;
    bool primed = false;

    // Samples one slot needs at this rate: 10 Hz plus the interpolator's taps.
    static int capacityFor (double sampleRate) noexcept;
    void reset() noexcept;
    float process (float in, double hz, double feedback, double feedforward, double sampleRate) noexcept;
};

// ---------------------------------------------------------------------------
// bus_mb(l, r, base, mix): 3-band upward + downward compressor, stereo, master
// only. Settings are 19 values in mem[] from `base`, read every kInterval
// samples (see api_reference.md for the layout):
//   [0] crossover 1 Hz, [1] crossover 2 Hz, [2] attack ms, [3] release ms,
//   then for the low, mid and high band (5 values each, from base + 4 + 5 * band):
//   lower threshold dB, upper threshold dB, upward ratio, downward ratio, output gain dB.
// Bands: Linkwitz-Riley 4th order splits (two Butterworth biquads per side),
// with the low band through the second split's allpass, so the three bands
// sum to an allpass: flat magnitude.
// Detector: per band and per channel (not linked). The square of the band
// passes two symmetric one-poles (each half the attack time, but at least
// one cycle of the band's lowest frequency: 30 Hz, crossover 1, crossover 2,
// so a steady tone's level doesn't ripple), then a one-pole with the attack
// time constant while rising and the release one while falling. A follower
// with different rise and fall times on the raw square would settle well
// above its mean (about 2 dB at attack 10 ms / release 100 ms, measured).
// The level reads RMS + 3.01 dB, so a sine reads its peak level in dBFS.
// Levels below kFloorDb count as kFloorDb. The curve below is evaluated
// every kGainInterval (4) samples and the linear gain ramps between.
// Static curve per band, in dB, for level L:
//   gain = out_gain + (L < lo ? (lo - L) * (1 - 1/up_ratio) : 0)
//                   - (L > hi ? (L - hi) * (1 - 1/down_ratio) : 0)
// `mix` 0-1 blends each band's linear gain g with 1: the band is scaled by 1 + mix * (g - 1), not by
// mix times the gain in dB (mix 0.5 on +12 dB gives +7.9 dB); mix 0, or settings that
// can't be read, fade to the dry input over 10 ms and then pass it bit for bit.

// Biquad coefficients (a0 = 1), shared by both channels.
struct MbCoef
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

struct MbCompState
{
    static constexpr int kNumBands = 3;
    static constexpr int kNumValues = 4 + 5 * kNumBands;  // 19
    static constexpr int kInterval = 16;                  // settings are read every kInterval samples
    static constexpr int kGainInterval = 4;               // gains are computed every 4 samples, ramped between
    static constexpr double kFloorDb = -90.0;

    // Valid ranges; a finite value outside is clamped, a non-finite one
    // takes the default (all defaults together leave the signal unchanged).
    static constexpr double kMinXoverHz = 20.0;
    static constexpr double kMaxXoverHz = 20000.0;  // and at most 0.45 * rate
    static constexpr double kMinAttackMs = 0.1, kMaxAttackMs = 1000.0;
    static constexpr double kMinReleaseMs = 1.0, kMaxReleaseMs = 5000.0;
    static constexpr double kMinThresholdDb = -90.0, kMaxThresholdDb = 12.0;
    static constexpr double kMinRatio = 1.0, kMaxRatio = 100.0;
    static constexpr double kMinGainDb = -48.0, kMaxGainDb = 48.0;

    // The split: low = LP4(f1) then allpass(f2); rest = HP4(f1); mid = LP4(f2) of rest;
    // high = HP4(f2) of rest. LP4/HP4 are two identical Butterworth stages.
    enum Stage { kLo1, kLo2, kLoAp, kRest1, kRest2, kMid1, kMid2, kHi1, kHi2, kNumStages };
    MbCoef lp1, ap2, hp1, lp2, hp2;  // at f1 (lp1, hp1) and f2 (ap2, lp2, hp2)
    double z1[kNumStages][2] = {};   // [stage][channel]
    double z2[kNumStages][2] = {};
    double ms1[kNumBands][2] = {};   // symmetric mean-square stages
    double ms2[kNumBands][2] = {};
    double env[kNumBands][2] = {};   // attack/release stage
    double gain[kNumBands][2] = {};  // linear, ramping to the last computed value
    double step[kNumBands][2] = {};
    int gainCountdown = 0;

    double raw[kNumValues] = {};     // last values read (bits), to skip recomputing
    bool haveRaw = false;
    bool readable = false;           // the last read found valid settings
    // Derived settings.
    double f1 = -1.0, f2 = -1.0;
    double atk = 1.0, rel = 1.0;
    double avg[kNumBands] = { 1.0, 1.0, 1.0 };  // symmetric stages' coefficient per band
    double attackMs = 10.0;
    double lowDb[kNumBands] = {}, highDb[kNumBands] = {};
    double upSlope[kNumBands] = {}, downSlope[kNumBands] = {}, gainDb[kNumBands] = {};
    double wet = 0.0, wetStep = 1.0;
    int countdown = 0;
    double sampleRate = 48000.0;

    void init (double newSampleRate) noexcept;
    void reset() noexcept;
    // One stereo step; `vm` is the master VM (its mem[] holds the settings).
    void process (void* vm, float inL, float inR, double base, double mix, float& outL, float& outR) noexcept;

    // The static curve (dB) for one band at a level in dB: what the
    // smoketest checks the rendered gains against.
    double curveDb (int band, double levelDb) const noexcept;

private:
    void readSettings (void* vm, double base) noexcept;
    void setCrossovers (double newF1, double newF2) noexcept;
    void setAveraging() noexcept;  // after attackMs or the crossovers change
};

// Fast log2 for the compressor's per-sample level (absolute error below
// 1e-7; the smoketest checks it against std::log2). x must be a normal,
// finite, positive double (the caller floors it). 2^x is fastExp2.
inline double mbLog2 (double x) noexcept
{
    uint64_t bits = 0;
    std::memcpy (&bits, &x, sizeof (x));
    int e = static_cast<int> ((bits >> 52) & 0x7FFu) - 1023;
    bits = (bits & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull;  // mantissa in [1, 2)
    double m = 0.0;
    std::memcpy (&m, &bits, sizeof (m));
    if (m > 1.4142135623730951)
    {
        m *= 0.5;
        ++e;
    }
    // ln m = 2 atanh(t), t = (m - 1) / (m + 1), |t| <= 0.172.
    const double t = (m - 1.0) / (m + 1.0);
    const double t2 = t * t;
    const double ln = 2.0 * t * (1.0 + t2 * (1.0 / 3.0 + t2 * (1.0 / 5.0 + t2 * (1.0 / 7.0 + t2 * (1.0 / 9.0)))));
    return static_cast<double> (e) + ln * 1.4426950408889634;
}

// 2^x, clamped to +-60 (the same series as NodeOverrides' fastExp2, kept here
// so the engine effects don't depend on the override layer).
inline double mbExp2 (double x) noexcept
{
    x = x > -60.0 ? x : -60.0;
    x = x < 60.0 ? x : 60.0;
    const int i = static_cast<int> (x + 1024.5) - 1024;
    const double f = (x - static_cast<double> (i)) * 0.69314718055994530942;
    const double p = 1.0 + f * (1.0 + f * (0.5 + f * (1.0 / 6.0 + f * (1.0 / 24.0 + f * (1.0 / 120.0 + f * (1.0 / 720.0))))));
    const uint64_t bits = static_cast<uint64_t> (i + 1023) << 52;
    double scale = 0.0;
    std::memcpy (&scale, &bits, sizeof (scale));
    return p * scale;
}

} // namespace vibesynth
