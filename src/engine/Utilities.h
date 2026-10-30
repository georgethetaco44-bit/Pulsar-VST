// M4 — Utilities.h: own-DSP helpers for the lexicon (SPEC §5.2), part 1.
//
// Everything here is header-only, allocation-free and audio-thread safe.
// Real-time rules (SPEC §11): no allocation/locks/logging/I/O/exceptions;
// NaN/Inf checks use bit-pattern tests, never std::isnan.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace vibesynth {

// ---------------------------------------------------------------------------
// Bit-pattern double checks (duplicated from patch/PatchEngine.h so the
// engine headers stay self-contained for the CLI and future workers).

inline bool isFiniteBitsD (double v) noexcept
{
    uint64_t bits = 0;
    static_assert (sizeof (bits) == sizeof (v), "double must be 64-bit");
    std::memcpy (&bits, &v, sizeof (v));
    return ((bits >> 52) & 0x7FFu) != 0x7FFu;
}

inline float sanitizeFloat (float v) noexcept
{
    uint32_t bits = 0;
    static_assert (sizeof (bits) == sizeof (v), "float must be 32-bit");
    std::memcpy (&bits, &v, sizeof (v));
    if (((bits >> 23) & 0xFFu) == 0xFFu)
        return 0.0f;
    return v;
}

// ---------------------------------------------------------------------------
// Clamping / mapping helpers (SPEC §5.1 rules 1–2: normalized 0–1 inputs).

inline double clamp01 (double v) noexcept
{
    if (! isFiniteBitsD (v))
        return 0.0;
    if (v < 0.0)
        return 0.0;
    if (v > 1.0)
        return 1.0;
    return v;
}

inline double clampHz (double hz, double sampleRate) noexcept
{
    if (! isFiniteBitsD (hz))
        return 0.0;
    if (hz < 0.0)
        return 0.0;
    const double hi = 0.45 * sampleRate;
    return hz > hi ? hi : hz;
}

// Exponential 20 Hz – 20 kHz map for normalized filter cutoff (SPEC §5.2).
inline double cutoffFromNorm (double c, double sampleRate) noexcept
{
    const double norm = clamp01 (c);
    double hz = 20.0 * std::pow (1000.0, norm);
    return clampHz (hz, sampleRate);
}

// Normalized 0–1 → 1 ms – 10 s exponential map for ADSR times (SPEC §5.2).
inline double secondsFromNorm (double t) noexcept
{
    const double norm = clamp01 (t);
    return 0.001 * std::pow (10000.0, norm);
}

// Clamp a slot index to [0, numSlots); out-of-range clamps to 0 (SPEC §5.1).
inline int clampSlot (double slot, int numSlots) noexcept
{
    if (! isFiniteBitsD (slot))
        return 0;
    const int s = static_cast<int> (slot);
    if (s < 0 || s >= numSlots)
        return 0;
    return s;
}

// ---------------------------------------------------------------------------
// Cheap approximations (SPEC §4.2.1 wrapper rules). Callers sanitize inputs
// first (bit-pattern NaN guards); the smoketest checks the max error of
// each against std::.

// Padé [7/6] tanh, input clamped at ±4.97 where it reaches 1.0.
// Max |error| vs std::tanh ≈ 1e-4 (−80 dB).
inline double fastTanh (double x) noexcept
{
    constexpr double kClamp = 4.97;
    if (x > kClamp) x = kClamp;
    else if (x < -kClamp) x = -kClamp;
    const double x2 = x * x;
    const double num = x * (135135.0 + x2 * (17325.0 + x2 * (378.0 + x2)));
    const double den = 135135.0 + x2 * (62370.0 + x2 * (3150.0 + 28.0 * x2));
    const double y = num / den;
    return y > 1.0 ? 1.0 : (y < -1.0 ? -1.0 : y);
}

// sin via range reduction to [−π/2, π/2] and a 9th-order odd polynomial.
// Max |error| vs std::sin ≈ 4e-6. x must be finite.
inline double fastSin (double x) noexcept
{
    constexpr double kTwoPi = 6.28318530717958647692;
    double t = x * (1.0 / kTwoPi);
    t -= std::floor (t + 0.5);        // [−0.5, 0.5] cycles
    if (t > 0.25) t = 0.5 - t;        // sin(π − a) = sin(a)
    else if (t < -0.25) t = -0.5 - t;
    const double y = kTwoPi * t;
    const double y2 = y * y;
    return y * (1.0 + y2 * (-1.0 / 6.0 + y2 * (1.0 / 120.0
                + y2 * (-1.0 / 5040.0 + y2 * (1.0 / 362880.0)))));
}

} // namespace vibesynth (part 1: finite/clamp/map helpers)
