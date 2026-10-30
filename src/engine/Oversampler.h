// M7 — Oversampler.h: the 2x voice path's decimator (SPEC §4.2).
//
// Voices render at 2x the host rate straight into one mono sum, so a
// single decimator brings that sum back to the host rate. It is a
// polyphase IIR half-band: two chains of first-order allpasses (in z^2)
// running at the host rate,
//
//     H(z) = 0.5 * (A0(z^2) + z^-1 * A1(z^2)),
//
// with the coefficients designed at init() by the classic elliptic
// half-band method (Valenzuela & Constantinides 1983, in the closed form
// popularised by Laurent de Soras' HIIR notes): given a stopband
// attenuation and a transition width, compute the elliptic modulus q and
// the order, then each coefficient from two rapidly converging theta-type
// series. Coefficients alternate between the chains (even -> A0).
//
// Design point: transition 0.05 of the 2x rate (passband to 0.225 * 2x =
// 0.45 * host, stopband from 0.275 * 2x = 0.55 * host), designed for
// 100 dB, which yields 8 coefficients. Measured by the smoketest: >= 100 dB
// rejection above 0.55 * host, ripple < 1e-6 dB to 20 kHz at 48 kHz.
//
// Header-only, allocation-free after init(); process() is audio-thread
// safe (SPEC §11). State is double: one decimator per player, so precision
// is free.

#pragma once

#include <cmath>

namespace vibesynth {

class HalfbandDecimator
{
public:
    static constexpr int kMaxCoefs = 12;
    static constexpr double kDesignAttenuationDb = 100.0;
    static constexpr double kDesignTransition = 0.05; // of the 2x rate

    HalfbandDecimator() { init(); }

    // Off the audio thread: designs the coefficients and clears the state.
    void init (double attenuationDb = kDesignAttenuationDb,
               double transition = kDesignTransition) noexcept
    {
        numCoefs_ = design (attenuationDb, transition, coefs_);
        reset();
    }

    void reset() noexcept
    {
        for (int i = 0; i < kMaxCoefs; ++i)
            x1_[i] = y1_[i] = 0.0;
    }

    int numCoefs() const noexcept { return numCoefs_; }
    double coef (int i) const noexcept { return coefs_[i]; }

    // One host-rate output from two 2x-rate inputs, `older` then `newer`.
    // The newer sample feeds chain A0 (even coefficients), the older one
    // chain A1, so the pair is the polyphase split of H above.
    float process (float older, float newer) noexcept
    {
        double a = static_cast<double> (newer);
        double b = static_cast<double> (older);
        for (int i = 0; i < numCoefs_; i += 2)
            a = stage (i, a);
        for (int i = 1; i < numCoefs_; i += 2)
            b = stage (i, b);
        return static_cast<float> (0.5 * (a + b));
    }

    // |H| and group delay (in 2x-rate samples) at `f`, a fraction of the
    // 2x rate. Analytic, for the smoketest and the report.
    void response (double f, double& magnitude, double& groupDelay) const noexcept
    {
        double re = 0.0, im = 0.0;
        evaluate (f, re, im);
        magnitude = std::sqrt (re * re + im * im);
        const double d = 1.0e-7;
        double r0 = 0.0, i0 = 0.0, r1 = 0.0, i1 = 0.0;
        evaluate (f - d, r0, i0);
        evaluate (f + d, r1, i1);
        double dphi = std::atan2 (i1, r1) - std::atan2 (i0, r0);
        const double pi = 3.14159265358979323846;
        while (dphi > pi) dphi -= 2.0 * pi;
        while (dphi < -pi) dphi += 2.0 * pi;
        groupDelay = -dphi / (2.0 * pi * 2.0 * d);
    }

private:
    // First-order allpass in z^2, run at the host rate:
    // y[n] = a * (x[n] - y[n-1]) + x[n-1].
    double stage (int i, double x) noexcept
    {
        const double y = coefs_[i] * (x - y1_[i]) + x1_[i];
        x1_[i] = x;
        y1_[i] = y;
        return y;
    }

    void evaluate (double f, double& re, double& im) const noexcept
    {
        const double pi = 3.14159265358979323846;
        const double w = 2.0 * pi * f;
        // A(z^2) for each chain: product of (a + z^-2) / (1 + a z^-2).
        double a0r = 1.0, a0i = 0.0, a1r = 1.0, a1i = 0.0;
        const double c2 = std::cos (2.0 * w), s2 = -std::sin (2.0 * w); // z^-2
        for (int i = 0; i < numCoefs_; ++i)
        {
            const double a = coefs_[i];
            const double nr = a + c2, ni = s2;
            const double dr = 1.0 + a * c2, di = a * s2;
            const double den = dr * dr + di * di;
            const double qr = (nr * dr + ni * di) / den;
            const double qi = (ni * dr - nr * di) / den;
            double& pr = (i % 2 == 0) ? a0r : a1r;
            double& pi_ = (i % 2 == 0) ? a0i : a1i;
            const double tr = pr * qr - pi_ * qi;
            pi_ = pr * qi + pi_ * qr;
            pr = tr;
        }
        // z^-1 * A1
        const double c1 = std::cos (w), s1 = -std::sin (w);
        const double br = a1r * c1 - a1i * s1;
        const double bi = a1r * s1 + a1i * c1;
        re = 0.5 * (a0r + br);
        im = 0.5 * (a0i + bi);
    }

    // Elliptic half-band design; returns the number of coefficients.
    static int design (double attenuationDb, double transition, double* out) noexcept
    {
        const double pi = 3.14159265358979323846;
        double k = std::tan ((1.0 - 2.0 * transition) * pi / 4.0);
        k *= k;
        const double kksqrt = std::pow (1.0 - k * k, 0.25);
        const double e = 0.5 * (1.0 - kksqrt) / (1.0 + kksqrt);
        const double e2 = e * e;
        const double e4 = e2 * e2;
        const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));

        const double attn = std::pow (10.0, -attenuationDb / 10.0);
        const double a = attn / (1.0 - attn);
        int order = static_cast<int> (std::ceil (std::log (a * a / 16.0) / std::log (q)));
        if (order % 2 == 0)
            ++order;
        int n = (order - 1) / 2;
        if (n > kMaxCoefs)
            n = kMaxCoefs;
        if (n < 1)
            n = 1;

        for (int index = 0; index < n; ++index)
        {
            const int c = index + 1;
            double num = 0.0;
            {
                double sign = 1.0, term = 0.0;
                for (int i = 0; i < 64; ++i, sign = -sign)
                {
                    term = std::pow (q, static_cast<double> (i * (i + 1)))
                         * std::sin ((2 * i + 1) * c * pi / order) * sign;
                    num += term;
                    if (std::fabs (term) <= 1.0e-100)
                        break;
                }
            }
            num *= std::pow (q, 0.25);
            double den = 0.0;
            {
                double sign = -1.0, term = 0.0;
                for (int i = 1; i < 64; ++i, sign = -sign)
                {
                    term = std::pow (q, static_cast<double> (i * i))
                         * std::cos (2 * i * c * pi / order) * sign;
                    den += term;
                    if (std::fabs (term) <= 1.0e-100)
                        break;
                }
            }
            den += 0.5;
            const double ww = num / den;
            const double wwsq = ww * ww;
            const double x = std::sqrt ((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq);
            out[index] = (1.0 - x) / (1.0 + x);
        }
        return n;
    }

    double coefs_[kMaxCoefs] = {};
    double x1_[kMaxCoefs] = {};
    double y1_[kMaxCoefs] = {};
    int numCoefs_ = 0;
};

} // namespace vibesynth
