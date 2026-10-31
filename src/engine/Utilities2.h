// M4 — Utilities.h part 2: stateless shapers, PolyBLEP, supersaw,
// envelope follower.
#pragma once

#include <cmath>

#include "engine/Utilities.h"

namespace vibesynth {

// ---------------------------------------------------------------------------
// First-order ADAA state & stateless shapers (fx_fold, fx_sat / bus_sat, fx_shape).

constexpr int kMaxAdaaCalls = 16;
constexpr int kNumAdaaGroups = 4; // 0: FxFold, 1: FxSat, 2: FxShape, 3: BusSat

struct AdaaSlot
{
    double prevX = 0.0;
    bool hasPrev = false;

    void reset() noexcept
    {
        prevX = 0.0;
        hasPrev = false;
    }
};

// Antiderivative of tanh(u): ln(cosh(u)) via Padé [10/8] in u^2 on |u| < 5,
// C^1-matched to |u| - ln(2) for |u| >= 5 (max error 1.7e-6, no libm log/cosh).
inline double adaaTanhIntegral (double u) noexcept
{
    const double a = std::fabs (u);
    if (a >= 5.0)
        return a - 0.6931017134612381;
    const double t = a * a;
    const double num = t * (0.50001217246872898 + t * (0.21305684996958907
                     + t * (0.021913357538720906 + t * (0.00051997837295542299
                     + t * 1.2736434834266683e-06))));
    const double den = 1.0 + t * (0.59282298366242903 + t * (0.098172067261324242
                     + t * (0.0045311349598787781 + t * 3.9401989534643642e-05)));
    return num / den;
}

inline float foldSample (float x, double amount, AdaaSlot* slot = nullptr) noexcept
{
    const double a = clamp01 (amount);
    const double scale = 1.0 + 8.0 * a;
    const double xc = clampSafe (static_cast<double> (sanitizeFloat (x)), 16.0);
    if (slot == nullptr)
        return static_cast<float> (fastSin (xc * scale));
    if (! slot->hasPrev)
    {
        slot->prevX = xc;
        slot->hasPrev = true;
        return static_cast<float> (fastSin (xc * scale));
    }
    const double x0 = slot->prevX;
    slot->prevX = xc;
    const double u1 = xc * scale;
    const double u0 = x0 * scale;
    const double du = u1 - u0;
    if (std::fabs (du) < 1.0e-5)
        return static_cast<float> (fastSin (0.5 * (u1 + u0)));
    const double f1 = fastSin (u1 - kHalfPi);
    const double f0 = fastSin (u0 - kHalfPi);
    return static_cast<float> (clampSafe ((f1 - f0) / du, 1.0));
}

// Gain-compensated tanh saturation: k = 1 + 9·drive, unity-ish at drive 0.
inline float satSample (float x, double drive, AdaaSlot* slot = nullptr) noexcept
{
    const double d = clamp01 (drive);
    const double k = 1.0 + 9.0 * d;
    const double comp = 0.35 + 0.65 / std::sqrt (k);
    const double xc = clampSafe (static_cast<double> (sanitizeFloat (x)), 16.0);
    if (slot == nullptr)
        return static_cast<float> (fastTanh (xc * k) * comp);
    if (! slot->hasPrev)
    {
        slot->prevX = xc;
        slot->hasPrev = true;
        return static_cast<float> (fastTanh (xc * k) * comp);
    }
    const double x0 = slot->prevX;
    slot->prevX = xc;
    const double u1 = xc * k;
    const double u0 = x0 * k;
    const double du = u1 - u0;
    double shaped = 0.0;
    if (std::fabs (du) < 1.0e-5)
        shaped = fastTanh (0.5 * (u1 + u0));
    else
        shaped = clampSafe ((adaaTanhIntegral (u1) - adaaTanhIntegral (u0)) / du, 1.0);
    return static_cast<float> (shaped * comp);
}

inline double shapeCurveEval (double u, int curve) noexcept
{
    switch (curve)
    {
        case 0: // Hard clip
            return u > 1.0 ? 1.0 : (u < -1.0 ? -1.0 : u);
        case 1: // Asymmetric tube
            return u >= 0.0 ? fastTanh (u) : 0.7 * fastTanh (u * (1.0 / 0.7));
        case 2: // Half-wave rectify
            return u > 0.0 ? fastTanh (u) : 0.0;
        case 3: // Full-wave rectify
            return std::fabs (fastTanh (u));
        case 4: // Chebyshev-3
            if (u > 1.0) return -1.0;
            if (u < -1.0) return 1.0;
            return 3.0 * u - 4.0 * u * u * u;
        default: // 5: Diode foldback
            return fastSin (u) + 0.25 * fastSin (2.0 * u + kHalfPi) - 0.25;
    }
}

inline double shapeCurveIntegral (double u, int curve) noexcept
{
    const double a = std::fabs (u);
    switch (curve)
    {
        case 0: // Hard clip
            return a <= 1.0 ? (0.5 * u * u) : (a - 0.5);
        case 1: // Asymmetric tube
            return u >= 0.0 ? adaaTanhIntegral (u) : 0.49 * adaaTanhIntegral (u * (1.0 / 0.7));
        case 2: // Half-wave rectify
            return u > 0.0 ? adaaTanhIntegral (u) : 0.0;
        case 3: // Full-wave rectify
            return (u >= 0.0 ? 1.0 : -1.0) * adaaTanhIntegral (u);
        case 4: // Chebyshev-3
            if (a <= 1.0)
            {
                const double u2 = u * u;
                return 1.5 * u2 - u2 * u2;
            }
            return 1.5 - a;
        default: // 5: Diode foldback
            return fastSin (u - kHalfPi) + 0.125 * fastSin (2.0 * u) - 0.25 * u;
    }
}

inline float shapeSample (float x, double drive, double type, AdaaSlot* slot = nullptr) noexcept
{
    const double d = clamp01 (drive);
    const double k = 1.0 + 9.0 * d;
    const double comp = 0.45 + 0.55 / std::sqrt (k);
    const double tNorm = clamp01 (type);
    int curve = static_cast<int> (tNorm * 6.0);
    if (curve > 5) curve = 5;
    if (curve < 0) curve = 0;

    const double xc = clampSafe (static_cast<double> (sanitizeFloat (x)), 16.0);
    if (slot == nullptr)
        return static_cast<float> (clampSafe (shapeCurveEval (xc * k, curve) * comp, 1.5));
    if (! slot->hasPrev)
    {
        slot->prevX = xc;
        slot->hasPrev = true;
        return static_cast<float> (clampSafe (shapeCurveEval (xc * k, curve) * comp, 1.5));
    }
    const double x0 = slot->prevX;
    slot->prevX = xc;
    const double u1 = xc * k;
    const double u0 = x0 * k;
    const double du = u1 - u0;
    double shaped = 0.0;
    if (std::fabs (du) < 1.0e-5)
        shaped = shapeCurveEval (0.5 * (u1 + u0), curve);
    else
        shaped = clampSafe ((shapeCurveIntegral (u1, curve) - shapeCurveIntegral (u0, curve)) / du, 1.5);
    return static_cast<float> (clampSafe (shaped * comp, 1.5));
}

// ---------------------------------------------------------------------------
// Supersaw voice (own; 7 detuned PolyBLEP saws, centre + 3 pairs).

struct SupersawState
{
    static constexpr int kNumPairs = 3;
    static constexpr int kNumSides = kNumPairs * 2;
    double phase = 0.0;                     // centre saw phase, cycles
    double pairPhase[kNumSides] = {};       // detuned pair phases, cycles
    double seedPhase[kNumSides] = {};       // per-voice start phases (init)

    // Back to the seeded start phases (seeded once in VoiceRack::init so
    // the 7 saws never start phase-aligned).
    void reset() noexcept
    {
        phase = 0.0;
        for (int i = 0; i < kNumSides; ++i)
            pairPhase[i] = seedPhase[i];
    }
};

// One PolyBLEP-corrected saw sample from a cycle phase in [0, 1).
inline float polyblepSaw (double phase, double phaseInc) noexcept
{
    double v = 2.0 * phase - 1.0;
    if (phase < phaseInc)
    {
        const double t = phase / phaseInc;
        v += t + t - t * t - 1.0;
    }
    else if (phase > 1.0 - phaseInc)
    {
        const double t = (phase - 1.0) / phaseInc + 1.0;
        v -= t + t - t * t - 1.0;
    }
    return static_cast<float> (v);
}

inline float supersawProcess (SupersawState& st, double hz, double detune,
                              double mix, double sampleRate) noexcept
{
    const double d = clamp01 (detune) * 0.017; // ±30 cents ≈ ±1.7 % at 1.0
    const double m = clamp01 (mix);
    const double h = clampHz (hz, sampleRate);
    const double inc = h / sampleRate;

    st.phase += inc;
    st.phase -= std::floor (st.phase);
    const float centre = polyblepSaw (st.phase, inc);

    double sideSum = 0.0;
    for (int p = 0; p < SupersawState::kNumPairs; ++p)
    {
        const double spread = d * static_cast<double> (p + 1) / 3.0;
        for (int s = 0; s < 2; ++s)
        {
            const double f = (s == 0) ? (1.0 - spread) : (1.0 + spread);
            double& ph = st.pairPhase[p * 2 + s];
            ph += inc * f;
            ph -= std::floor (ph);
            sideSum += static_cast<double> (polyblepSaw (ph, inc * f));
        }
    }
    // Crossfade centre → mean of the 6 detuned saws. The side mean of 6
    // decorrelated saws has ~1/sqrt(6) of a single saw's RMS; compensate
    // with 1/sqrt((1-m)^2 + m^2/3) (partial, so aligned-phase peaks stay
    // ≤ ~1.73 before the final 0.55 trim; full compensation would be /6).
    const double sideMean = sideSum / static_cast<double> (SupersawState::kNumSides);
    const double mixed = static_cast<double> (centre) * (1.0 - m) + sideMean * m;
    const double comp = 1.0 / std::sqrt ((1.0 - m) * (1.0 - m) + m * m / 3.0);
    return static_cast<float> (mixed * comp * 0.55);
}

// ---------------------------------------------------------------------------
// Envelope follower (own; fast attack, slow release on |in|).

struct EnvFollowState
{
    double level = 0.0;
    double aCoef = 1.0; // 1 ms attack, set by init()
    double rCoef = 1.0; // 100 ms release, set by init()

    // Off the audio thread (VoiceRack::init): precompute the exp() coefs.
    void init (double sampleRate) noexcept
    {
        aCoef = 1.0 - std::exp (-1.0 / (0.001 * sampleRate));
        rCoef = 1.0 - std::exp (-1.0 / (0.100 * sampleRate));
        level = 0.0;
    }

    void reset() noexcept { level = 0.0; }

    float process (float in) noexcept
    {
        const double target = in < 0.0f ? -static_cast<double> (in)
                                        : static_cast<double> (in);
        const double coef = (target > level) ? aCoef : rCoef;
        level += coef * (target - level);
        return static_cast<float> (level);
    }
};

} // namespace vibesynth
