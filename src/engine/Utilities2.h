// M4 — Utilities.h part 2: stateless shapers, PolyBLEP, supersaw,
// envelope follower.
#pragma once

#include <cmath>

#include "engine/Utilities.h"

namespace vibesynth {

// ---------------------------------------------------------------------------
// Stateless shapers (fx_fold, fx_sat / bus_sat).

inline float foldSample (float x, double amount) noexcept
{
    const double a = clamp01 (amount);
    const double scaled = static_cast<double> (x) * (1.0 + 8.0 * a);
    return static_cast<float> (fastSin (scaled));
}

// Gain-compensated tanh saturation: k = 1 + 9·drive, unity-ish at drive 0.
inline float satSample (float x, double drive) noexcept
{
    const double d = clamp01 (drive);
    const double k = 1.0 + 9.0 * d;
    const double v = static_cast<double> (x) * k;
    const double t = fastTanh (v);
    const double comp = 0.35 + 0.65 / std::sqrt (k);
    return static_cast<float> (t * comp);
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
