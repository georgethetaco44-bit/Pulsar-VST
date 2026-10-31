#pragma once

// M12 — Filters2.h: flt_peak, flt_formant, and flt_tilt (flt_notch uses
// SvfState::Notch in Filters.h). Header-only, allocation-free, audio-thread safe.

#include <cmath>

#include "engine/Filters.h"

namespace vibesynth {

constexpr int kNumFormantSlots = 2;
constexpr int kNumNotchSlots = 3;
constexpr int kNumPeakSlots = 3;
constexpr int kNumTiltSlots = 2;

// ---------------------------------------------------------------------------
// flt_peak(slot, in, cutoff, gain, res): TPT bell/peaking filter.
// gain 0..1 maps to -18..+18 dB (0.5 = 0 dB flat).
struct PeakSvfState
{
    double ic1 = 0.0, ic2 = 0.0;
    double cut = -1.0;
    double gainNorm = -1.0;
    double k0 = -1.0;
    double rawRes = -1.0;
    double k0Target = 2.0;
    double cacheCut = -2.0, cacheGain = -2.0, cacheK0 = -2.0;
    double a1 = 0.0, a2 = 0.0, a3 = 0.0, m1 = 0.0;

    void reset() noexcept { *this = PeakSvfState(); }

    float process (float in, double cutoffIn, double gainIn, double resIn,
                   const FilterRate& fr) noexcept
    {
        cut = smoothTowards (cut, clamp01 (cutoffIn), fr.smooth);
        gainNorm = smoothTowards (gainNorm, clamp01 (gainIn), fr.smooth);
        if (! sameBitsD (resIn, rawRes))
        {
            rawRes = resIn;
            const double r = clamp01 (resIn);
            const double kk = 2.0 * (1.0 - std::sqrt (std::sqrt (r)));
            k0Target = kk > kSvfMinDamping ? kk : kSvfMinDamping;
        }
        k0 = smoothTowards (k0, k0Target, fr.smooth);
        if (! sameBitsD (cut, cacheCut) || ! sameBitsD (gainNorm, cacheGain) || ! sameBitsD (k0, cacheK0))
        {
            cacheCut = cut;
            cacheGain = gainNorm;
            cacheK0 = k0;
            const double db = (gainNorm - 0.5) * 36.0; // -18..+18 dB
            // 10^(db/40) = exp2(db * log2(10) / 40)
            const double A = std::exp2 (db * (3.32192809488736234787 / 40.0));
            const double g = fr.g (cut);
            const double k = k0 / A;
            a1 = 1.0 / (1.0 + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
            m1 = k * (A * A - 1.0);
        }
        const double v0 = static_cast<double> (in);
        const double v3 = v0 - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        const double out = v0 + m1 * v1;
        if (! isFiniteBitsD (out) || ! isFiniteBitsD (ic1) || ! isFiniteBitsD (ic2))
        {
            ic1 = ic2 = 0.0;
            return 0.0f;
        }
        return static_cast<float> (clampSafe (out));
    }
};

// ---------------------------------------------------------------------------
// flt_formant(slot, in, vowel, res): 3 parallel TPT band-passes morphing
// across a -> e -> i -> o -> u (vowel 0..1).
struct FormantState
{
    SvfState band[3];

    void reset() noexcept
    {
        for (int b = 0; b < 3; ++b)
            band[b].reset();
    }

    float process (float in, double vowelIn, double resIn, const FilterRate& fr) noexcept
    {
        // Precomputed normalized cutoffs c = ln(Hz / 20) / ln(1000) for F1, F2, F3:
        // a: (730, 1090, 2440 Hz), e: (530, 1840, 2480 Hz), i: (270, 2290, 3010 Hz),
        // o: (570, 840, 2410 Hz),  u: (300, 870, 2240 Hz).
        static constexpr double kVowelCuts[5][3] = {
            { 0.520467, 0.578460, 0.695163 }, // a
            { 0.474124, 0.654326, 0.697517 }, // e
            { 0.376895, 0.686089, 0.725624 }, // i
            { 0.484644, 0.540736, 0.693372 }, // o
            { 0.392146, 0.545815, 0.682894 }, // u
        };
        static constexpr double kBandWeights[3] = { 1.0, 0.65, 0.38 };

        const double v = clamp01 (vowelIn) * 4.0;
        int v0 = static_cast<int> (v);
        if (v0 >= 4) v0 = 3;
        if (v0 < 0) v0 = 0;
        const double vf = v - static_cast<double> (v0);

        const double r = clamp01 (resIn);
        // Map res 0..1 into vocal SVF resonance range 0.55..0.96 so formants stay distinct.
        const double svfRes = 0.55 + 0.41 * r;

        double sum = 0.0;
        for (int b = 0; b < 3; ++b)
        {
            const double c = (1.0 - vf) * kVowelCuts[v0][b] + vf * kVowelCuts[v0 + 1][b];
            const double bp = static_cast<double> (band[b].process (in, c, svfRes, fr, SvfState::Band));
            // Normalize by damping k so unity-ish peak grows mildly with res.
            const double normGain = band[b].k * (1.1 + 0.9 * r);
            sum += kBandWeights[b] * bp * normGain;
        }
        return static_cast<float> (clampSafe (sum * 0.75));
    }
};

// ---------------------------------------------------------------------------
// flt_tilt(slot, in, tilt): +-6 dB spectral tilt around 1 kHz (0.5 = flat).
struct TiltState
{
    double z = 0.0;
    double tilt = -1.0;
    double cacheTilt = -2.0;
    double gLo = 1.0, gHi = 1.0;

    void reset() noexcept { *this = TiltState(); }

    float process (float in, double tiltIn, const FilterRate& fr) noexcept
    {
        // 1 kHz in normalized cutoff: ln(1000 / 20) / ln(1000) = 0.5663233348
        constexpr double kPivotNorm = 0.5663233348183775;
        const double G = fr.ladderG (kPivotNorm);

        tilt = smoothTowards (tilt, clamp01 (tiltIn), fr.smooth);
        if (! sameBitsD (tilt, cacheTilt))
        {
            cacheTilt = tilt;
            const double bipolar = 2.0 * tilt - 1.0; // -1..+1 -> -6..+6 dB
            // 10^(6/20) = 1.99526231496888 -> log2(10^(6/20)) = 0.9965784284662087
            gHi = std::exp2 (bipolar * 0.9965784284662087);
            gLo = 1.0 / gHi;
        }

        const double x = static_cast<double> (in);
        const double v = (x - z) * G;
        const double lp = v + z;
        z = lp + v;
        const double hp = x - lp;
        const double out = gLo * lp + gHi * hp;
        if (! isFiniteBitsD (out) || ! isFiniteBitsD (z))
        {
            z = 0.0;
            return 0.0f;
        }
        return static_cast<float> (clampSafe (out));
    }
};

} // namespace vibesynth
