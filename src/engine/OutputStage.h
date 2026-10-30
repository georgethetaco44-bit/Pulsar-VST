// M5 — OutputStage.h: the always-on master output chain (SPEC §5.2, §7.2).
//
// trim -> DC block -> peak limiter -> hard clip +/-1 -> final non-finite
// check. It replaces M4's fixed -12 dB output gain. Header-only,
// allocation-free and audio-thread safe; state is owned by the processor,
// so it carries across patch swaps.
#pragma once

#include <cmath>

#include "engine/Utilities.h"

namespace vibesynth {

// One-pole DC blocker, ~5 Hz: inaudible on a 28 Hz sub (M5 test note).
struct DcBlocker
{
    double r = 0.9993;
    double x1 = 0.0;
    double y1 = 0.0;

    void init (double sampleRate) noexcept
    {
        constexpr double kTwoPi = 6.28318530717958647692;
        r = std::exp (-kTwoPi * 5.0 / (sampleRate > 0.0 ? sampleRate : 48000.0));
        reset();
    }

    void reset() noexcept { x1 = 0.0; y1 = 0.0; }

    double process (double x) noexcept
    {
        const double y = x - x1 + r * y1;
        x1 = x;
        y1 = y;
        return y;
    }
};

// Stereo-linked peak limiter: no lookahead (no latency), instant attack,
// ~80 ms exponential release, ceiling -0.3 dBFS.
struct PeakLimiter
{
    static constexpr double kCeiling = 0.9660508789898133; // -0.3 dBFS
    static constexpr double kReleaseSeconds = 0.080;

    double gain = 1.0;
    double releaseCoeff = 0.9997;

    void init (double sampleRate) noexcept
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        releaseCoeff = std::exp (-1.0 / (kReleaseSeconds * sr));
        reset();
    }

    void reset() noexcept { gain = 1.0; }

    void process (double& l, double& r) noexcept
    {
        const double peak = std::fmax (std::fabs (l), std::fabs (r));
        const double target = peak > kCeiling ? kCeiling / peak : 1.0;
        if (target < gain)
            gain = target;                                   // instant attack
        else
            gain = target + (gain - target) * releaseCoeff;  // release
        l *= gain;
        r *= gain;
    }
};

struct OutputStage
{
    DcBlocker dcL;
    DcBlocker dcR;
    PeakLimiter limiter;

    void init (double sampleRate) noexcept
    {
        dcL.init (sampleRate);
        dcR.init (sampleRate);
        limiter.init (sampleRate);
    }

    void reset() noexcept
    {
        dcL.reset();
        dcR.reset();
        limiter.reset();
    }

    // In: finite master output. Out: trimmed, DC-blocked, limited and
    // clipped samples; non-finite results become 0 and reset the chain.
    void process (float inL, float inR, float trim, float& outL, float& outR) noexcept
    {
        double l = dcL.process (static_cast<double> (inL) * trim);
        double r = dcR.process (static_cast<double> (inR) * trim);
        limiter.process (l, r);
        l = l > 1.0 ? 1.0 : (l < -1.0 ? -1.0 : l);
        r = r > 1.0 ? 1.0 : (r < -1.0 ? -1.0 : r);
        if (! isFiniteBitsD (l) || ! isFiniteBitsD (r))
        {
            reset();
            l = 0.0;
            r = 0.0;
        }
        outL = static_cast<float> (l);
        outR = static_cast<float> (r);
    }
};

} // namespace vibesynth
