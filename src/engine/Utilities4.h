// M4 — Utilities.h part 4: LR4 crossover, deterministic noise, bytebeat.
// XoverState method bodies live in LexiconRacks.cpp.
#pragma once

#include <cstdint>

#include "engine/Utilities.h"

namespace vibesynth {

// ---------------------------------------------------------------------------
// Linkwitz-Riley 4th-order crossover @ 100 Hz (own; two cascaded biquads).

struct XoverBand
{
    double b0 = 0.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; // a0 = 1
    double z1 = 0.0, z2 = 0.0;

    void reset() noexcept { z1 = 0.0; z2 = 0.0; }

    float process (float in) noexcept
    {
        const double x = static_cast<double> (in);
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return static_cast<float> (y);
    }
};

struct XoverState
{
    static constexpr double kFreq = 100.0;
    XoverBand lo1, lo2;
    XoverBand hi1, hi2;
    float lastHi = 0.0f;  // cached by processLo for util_xover_hi()

    void init (double sampleRate) noexcept;
    void reset() noexcept;
    float processLo (float in) noexcept;
};

// ---------------------------------------------------------------------------
// Deterministic per-voice white noise (xorshift32, seeded per voice).

struct NoiseState
{
    uint32_t s = 0x9E3779B9u;

    void seed (uint32_t seedValue) noexcept
    {
        s = (seedValue == 0u) ? 0x9E3779B9u : seedValue;
    }

    float next() noexcept
    {
        uint32_t x = s;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s = x;
        return static_cast<float> (static_cast<double> (x >> 8)
                                   / 8388608.0 - 1.0);
    }
};

// ---------------------------------------------------------------------------
// Bytebeat (own; integer ops in C++, SPEC §5.2).

inline float bytebeatSample (double t, double mode) noexcept
{
    if (! isFiniteBitsD (t))
        t = 0.0;
    const double mIn = (! isFiniteBitsD (mode) || mode < 0.0) ? 0.0 : mode;
    const int m = (mIn > 3.0) ? 3 : static_cast<int> (mIn);
    const int64_t tt = static_cast<int64_t> (t < 0.0 ? 0.0 : t);
    int64_t v = 0;
    switch (m)
    {
        case 0: v = tt * ((tt >> 12) & (tt >> 8)); break;
        case 1: v = tt * ((tt >> 10) & ((tt >> 5) | (tt >> 1))); break;
        case 2: v = (tt >> 6) ^ (tt & 0xFF) ^ ((tt >> 3) & 0x3F); break;
        default: v = (tt * (tt >> 5)) | (tt >> 4); break;
    }
    const double out = static_cast<double> (v & 0xFF) / 127.5 - 1.0;
    return sanitizeFloat (static_cast<float> (out));
}

} // namespace vibesynth
