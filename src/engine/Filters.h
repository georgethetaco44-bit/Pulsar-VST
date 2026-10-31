// M7 — Filters.h: own TPT state-variable filter and 4-pole ZDF ladder
// (SPEC §4.2.1 M7 plan, §5.2 flt_*). Replace DaisySP's Svf (cutoff could
// only step every 16 samples) and LadderFilter (4x oversampled inside).
//
// Both filters smooth their normalized inputs per sample and map them per
// sample, so a fast sweep can't zipper:
//   - cutoff 0–1 is smoothed (one-pole, ~0.5 ms) in the normalized domain
//     (i.e. in pitch), then mapped to the prewarped g = tan(pi hz / sr) of
//     20 Hz – 20 kHz (exponential, clamped to 0.45 x srate) through a
//     513-point table built at init, linearly interpolated (max cutoff
//     error 0.08 cent at 96 kHz, 0.4 cent near 20 kHz at 48 kHz; checked
//     by the smoketest). An exp2 + Padé tan
//     computed per sample measured 45 ns per swept SVF call on the Deck
//     (a long dependent chain); the table gives ~20 ns;
//   - resonance is mapped only when its raw input changes (cached on the
//     raw 0–1 value) and the mapped damping/feedback is smoothed the same
//     way.
// Coefficients are recomputed only while the smoothed values move; each
// filter does one division per sample, swept or not.
//
// SVF: Zavalishin's TPT form (Simper's trapezoidal integrators), one
// solve per sample, LP/HP/BP taps (BP unnormalized: peak gain = Q).
// Damping k = 2 (1 - res^0.25) (the Daisy curve the presets were tuned
// with), floored at kSvfMinDamping so res 1.0 is a high but finite Q at
// any cutoff; k > 0 and g > 0 keep the TPT SVF stable.
//
// Ladder: four TPT one-pole lowpasses in a zero-delay feedback loop
// (feedback k = 4.2 x res, so it self-oscillates from res ~0.95), solved
// linearly for the loop output, then the feedback goes through a Padé
// [3/2] tanh: u = comp * x - k * tanh(y4). |tanh| <= 1 bounds the stage input
// by |comp * x| + k, so self-oscillation stays bounded. comp = 1 + 0.5 k
// keeps the passband level like Daisy's passband gain 0.5.
//
// Header-only, allocation-free, audio-thread safe (SPEC §11).

#pragma once

#include <cmath>

#include "engine/Utilities.h"

namespace vibesynth {

// Padé [3/2] tanh of a / b (b > 0), clamped at ±3 where it reaches ±1:
// the ladder's feedback saturator (max error 2 % near ±1.6, monotonic).
// Taking the fraction lets the ladder fold its loop solve into this one
// division: tanh(a / b) = a (27 b^2 + a^2) / (b (27 b^2 + 9 a^2)).
inline double ladderTanh (double a, double b) noexcept
{
    if (a > 3.0 * b) return 1.0;
    if (a < -3.0 * b) return -1.0;
    const double a2 = a * a, b2 = 27.0 * b * b;
    return a * (b2 + a2) / (b * (b2 + 9.0 * a2));
}

// Per-rate constants shared by every filter slot of a rack: the smoothing
// coefficient and the cutoff -> g table (init() runs off the audio thread).
struct FilterRate
{
    static constexpr int kTableSteps = 512;
    static constexpr double kLog2Range = 9.965784284662087; // log2(1000)

    double sampleRate = 48000.0;
    double smooth = 1.0;     // one-pole coefficient (~0.5 ms)
    double gTable[kTableSteps + 2] = {};
    double ladderTable[kTableSteps + 2] = {}; // G = g / (1 + g)

    void init (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 48000.0;
        smooth = 1.0 - std::exp (-1.0 / (0.0005 * sampleRate));
        const double maxHz = 0.45 * sampleRate;
        for (int i = 0; i <= kTableSteps + 1; ++i)
        {
            double hz = 20.0 * std::exp2 (kLog2Range * i / kTableSteps);
            if (hz > maxHz)
                hz = maxHz;
            gTable[i] = std::tan (3.14159265358979323846 * hz / sampleRate);
            ladderTable[i] = gTable[i] / (1.0 + gTable[i]);
        }
    }

    // Prewarped g for a normalized cutoff c in [0, 1].
    double g (double c) const noexcept { return lookup (gTable, c); }
    // The TPT one-pole gain G = g / (1 + g) for the ladder.
    double ladderG (double c) const noexcept { return lookup (ladderTable, c); }

    static double lookup (const double* table, double c) noexcept
    {
        const double x = c * kTableSteps;
        const int i = static_cast<int> (x);
        const double f = x - i;
        return table[i] + f * (table[i + 1] - table[i]);
    }
};

// One-pole step that lands exactly once it is close (so the coefficient
// cache hits when the input is static). `cur < 0` means "unset": snap.
inline double smoothTowards (double cur, double target, double coeff) noexcept
{
    if (cur < 0.0)
        return target;
    const double d = target - cur;
    if (d < 1.0e-9 && d > -1.0e-9)
        return target;
    return cur + coeff * d;
}

constexpr double kSvfMinDamping = 0.05;   // res 1.0: Q = 20 (+26 dB peak)
constexpr double kLadderMaxFeedback = 4.2; // self-oscillation from res ~0.952

struct SvfState
{
    enum Tap { Low = 0, High = 1, Band = 2 };

    double ic1 = 0.0, ic2 = 0.0;   // integrator states
    double cut = -1.0;             // smoothed normalized cutoff (-1: unset)
    double k = -1.0;               // smoothed damping (-1: unset)
    double rawRes = -1.0;          // cache key for kTarget
    double kTarget = 2.0;
    double cacheCut = -2.0, cacheK = -2.0;
    double a1 = 0.0, a2 = 0.0, a3 = 0.0;

    void reset() noexcept { *this = SvfState(); }

    float process (float in, double cutoffIn, double resIn, const FilterRate& fr, int tap) noexcept
    {
        cut = smoothTowards (cut, clamp01 (cutoffIn), fr.smooth);
        if (! sameBitsD (resIn, rawRes))
        {
            rawRes = resIn;
            const double r = clamp01 (resIn);
            const double kk = 2.0 * (1.0 - std::sqrt (std::sqrt (r)));
            kTarget = kk > kSvfMinDamping ? kk : kSvfMinDamping;
        }
        k = smoothTowards (k, kTarget, fr.smooth);
        if (! sameBitsD (cut, cacheCut) || ! sameBitsD (k, cacheK))
        {
            cacheCut = cut;
            cacheK = k;
            const double g = fr.g (cut);
            a1 = 1.0 / (1.0 + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }
        const double v0 = static_cast<double> (in);
        const double v3 = v0 - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        const double out = tap == Low ? v2 : tap == Band ? v1 : v0 - k * v1 - v2;
        if (! isFiniteBitsD (out) || ! isFiniteBitsD (ic1) || ! isFiniteBitsD (ic2))
        {
            ic1 = ic2 = 0.0;
            return 0.0f;
        }
        return static_cast<float> (out);
    }
};

struct LadderState
{
    double s[4] = { 0.0, 0.0, 0.0, 0.0 }; // one-pole states
    double cut = -1.0;
    double k = -1.0;                      // smoothed feedback (-1: unset)
    double rawRes = -1.0;
    double kTarget = 0.0;
    double cacheCut = -2.0, cacheK = -2.0;
    double G = 0.0, G4 = 0.0, comp = 1.0, loop = 1.0; // loop = 1 + k G^4

    void reset() noexcept { *this = LadderState(); }

    float process (float in, double cutoffIn, double resIn, const FilterRate& fr) noexcept
    {
        cut = smoothTowards (cut, clamp01 (cutoffIn), fr.smooth);
        if (! sameBitsD (resIn, rawRes))
        {
            rawRes = resIn;
            kTarget = kLadderMaxFeedback * clamp01 (resIn);
        }
        k = smoothTowards (k, kTarget, fr.smooth);
        if (! sameBitsD (cut, cacheCut) || ! sameBitsD (k, cacheK))
        {
            cacheCut = cut;
            cacheK = k;
            G = fr.ladderG (cut);
            G4 = (G * G) * (G * G);
            loop = 1.0 + k * G4;
            comp = 1.0 + 0.5 * k;
        }
        const double x = comp * static_cast<double> (in);
        // Linear zero-delay solution for the loop output, y4 = (G^4 x + S)
        // / (1 + k G^4), then the feedback through tanh (the nonlinearity
        // bounds self-oscillation).
        const double S = (1.0 - G) * (((s[0] * G + s[1]) * G + s[2]) * G + s[3]);
        double y = x - k * ladderTanh (G4 * x + S, loop);
        for (int i = 0; i < 4; ++i)
        {
            const double v = (y - s[i]) * G;
            y = v + s[i];
            s[i] = y + v;
        }
        if (! isFiniteBitsD (y) || ! isFiniteBitsD (s[0]) || ! isFiniteBitsD (s[3]))
        {
            s[0] = s[1] = s[2] = s[3] = 0.0;
            return 0.0f;
        }
        return static_cast<float> (y);
    }
};

} // namespace vibesynth
