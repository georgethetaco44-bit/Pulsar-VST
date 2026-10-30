// M4 — LexiconRacks.cpp: Utilities3/Utilities4 out-of-line method bodies.
// (Rack init/reset/metering live in Racks.cpp / Racks2.cpp.)
//
// Real-time rules (SPEC §11): the per-sample paths below perform no
// allocation, locks, logging, I/O, exceptions or std::function; NaN/Inf
// checks use bit-pattern tests. All Init()/buffer sizing happens in
// init(), called off the audio thread from prepareToPlay.

#include "engine/Lexicon.h"
#include "engine/MasterRack.h"

#include <cmath>

namespace vibesynth {

// ---------------------------------------------------------------------------
// Utilities3 method bodies (CombState, SafeFbState).

float CombState::process (float in, double hz, double feedback,
                          double sampleRate) noexcept
{
    if (buf == nullptr || capacity < 4)
        return in;
    const double h = (hz < 10.0 || ! isFiniteBitsD (hz)) ? 10.0 : hz;
    double delay = sampleRate / h;
    const double maxDelay = static_cast<double> (capacity - 2);
    if (delay > maxDelay)
        delay = maxDelay;
    const int i0 = static_cast<int> (delay);
    const double frac = delay - static_cast<double> (i0);
    int r0 = writePos - i0;
    if (r0 < 0)
        r0 += capacity;
    int r1 = r0 - 1;
    if (r1 < 0)
        r1 += capacity;
    const float a = buf[r0];
    const float b = buf[r1];
    const float delayed = a + static_cast<float> (frac) * (b - a);
    const double fb = feedback < 0.0 ? 0.0 : (feedback > 0.95 ? 0.95 : feedback);
    buf[writePos] = in + delayed * static_cast<float> (fb);
    writePos += 1;
    if (writePos >= capacity)
        writePos = 0;
    return delayed;
}

float SafeFbState::process (float x, double sampleRate) noexcept
{
    if (! isFiniteBitsD (static_cast<double> (x)))
        x = 0.0f;
    const double r = 1.0 - 2.0 * 3.14159265358979323846 * 20.0 / sampleRate;
    const double rc = r < 0.0 ? 0.0 : (r > 0.9999 ? 0.9999 : r);
    const double xd = static_cast<double> (x);
    const double blocked = xd - dcPrevIn + rc * dcPrevOut;
    dcPrevIn = xd;
    dcPrevOut = blocked;
    double delta = blocked - last;
    if (delta > 0.25)
        delta = 0.25;
    else if (delta < -0.25)
        delta = -0.25;
    last = fastTanh (last + delta);
    return sanitizeFloat (static_cast<float> (last));
}

// Utilities4 XoverState method bodies.

void XoverState::init (double sampleRate) noexcept
{
    constexpr double kPi = 3.14159265358979323846;
    const double w0 = 2.0 * kPi * kFreq / sampleRate;
    const double cw = std::cos (w0);
    const double sw = std::sin (w0);
    const double Q = 0.7071067811865476;
    const double alpha = sw / (2.0 * Q);
    {
        const double b0 = (1.0 - cw) * 0.5;
        const double b1 = 1.0 - cw;
        const double b2 = b0;
        const double a0 = 1.0 + alpha;
        const double a1 = -2.0 * cw;
        const double a2 = 1.0 - alpha;
        XoverBand* stages[2] = { &lo1, &lo2 };
        for (int i = 0; i < 2; ++i)
        {
            stages[i]->b0 = b0 / a0; stages[i]->b1 = b1 / a0;
            stages[i]->b2 = b2 / a0;
            stages[i]->a1 = a1 / a0; stages[i]->a2 = a2 / a0;
        }
    }
    {
        const double b0 = (1.0 + cw) * 0.5;
        const double b1 = -(1.0 + cw);
        const double b2 = b0;
        const double a0 = 1.0 + alpha;
        const double a1 = -2.0 * cw;
        const double a2 = 1.0 - alpha;
        XoverBand* stages[2] = { &hi1, &hi2 };
        for (int i = 0; i < 2; ++i)
        {
            stages[i]->b0 = b0 / a0; stages[i]->b1 = b1 / a0;
            stages[i]->b2 = b2 / a0;
            stages[i]->a1 = a1 / a0; stages[i]->a2 = a2 / a0;
        }
    }
}

void XoverState::reset() noexcept
{
    lo1.reset(); lo2.reset(); hi1.reset(); hi2.reset();
    lastHi = 0.0f;
}

float XoverState::processLo (float in) noexcept
{
    const float hi = hi2.process (hi1.process (in));
    const float lo = lo2.process (lo1.process (in));
    lastHi = sanitizeFloat (hi);
    return sanitizeFloat (lo);
}

// ---------------------------------------------------------------------------
// Utilities5 OttState method bodies.

namespace {

// Butterworth (Q = 1/√2) biquads; two low-passes (or high-passes) in series
// make an LR4 side. The allpass is the LR4 pair's own phase (what LP4 + HP4
// sums to), used to keep the low band in phase with the upper two.
enum class BiquadKind { Lp, Hp, Ap };

void setButter (XoverBand& b, BiquadKind kind, double hz, double sampleRate) noexcept
{
    constexpr double kPi = 3.14159265358979323846;
    const double w0 = 2.0 * kPi * hz / sampleRate;
    const double cw = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * 0.7071067811865476);
    const double a0 = 1.0 + alpha;
    if (kind == BiquadKind::Lp)
    {
        b.b0 = (1.0 - cw) * 0.5 / a0; b.b1 = (1.0 - cw) / a0; b.b2 = b.b0;
    }
    else if (kind == BiquadKind::Hp)
    {
        b.b0 = (1.0 + cw) * 0.5 / a0; b.b1 = -(1.0 + cw) / a0; b.b2 = b.b0;
    }
    else
    {
        b.b0 = (1.0 - alpha) / a0; b.b1 = -2.0 * cw / a0; b.b2 = 1.0;
    }
    b.a1 = -2.0 * cw / a0;
    b.a2 = (1.0 - alpha) / a0;
}

// Band gain in dB for an envelope level in dB at depth 1 (curve above).
// Highest fast-peak level boost may lift a band to: where the makeup
// leaves a band sitting at the downward threshold (−18 + 4.5 dB).
constexpr double kBoostCeilDb = -13.5;
constexpr double kBoostCeil = 0.21134890398366465; // 10^(−13.5 / 20)

double ottGainDb (double levelDb) noexcept
{
    constexpr double kDownThresh = -18.0, kDownRatio = 4.0;
    constexpr double kUpThresh = -36.0, kUpRatio = 3.0, kMaxUp = 18.0;
    constexpr double kFloor = -60.0, kFloorFade = 18.0, kMakeup = 4.5;
    double g = kMakeup;
    if (levelDb > kDownThresh)
        g -= (levelDb - kDownThresh) * (1.0 - 1.0 / kDownRatio);
    else if (levelDb < kUpThresh)
    {
        double up = (kUpThresh - levelDb) * (1.0 - 1.0 / kUpRatio);
        if (up > kMaxUp) up = kMaxUp;
        if (levelDb < kFloor)
        {
            double f = 1.0 - (kFloor - levelDb) / kFloorFade;
            if (f < 0.0) f = 0.0;
            up *= f;
            g *= f; // no makeup on silence either
        }
        g += up;
    }
    return g;
}

} // namespace

void OttState::init (double sampleRate) noexcept
{
    setButter (loA, BiquadKind::Lp, 120.0, sampleRate);
    setButter (loB, BiquadKind::Lp, 120.0, sampleRate);
    setButter (restA, BiquadKind::Hp, 120.0, sampleRate);
    setButter (restB, BiquadKind::Hp, 120.0, sampleRate);
    setButter (loAp, BiquadKind::Ap, 2500.0, sampleRate);
    setButter (midA, BiquadKind::Lp, 2500.0, sampleRate);
    setButter (midB, BiquadKind::Lp, 2500.0, sampleRate);
    setButter (hiA, BiquadKind::Hp, 2500.0, sampleRate);
    setButter (hiB, BiquadKind::Hp, 2500.0, sampleRate);
    atk = 1.0 - std::exp (-1.0 / (0.010 * sampleRate));
    rel = 1.0 - std::exp (-1.0 / (0.100 * sampleRate));
    wetStep = 1.0 / (0.010 * sampleRate);
    reset();
}

void OttState::reset() noexcept
{
    loA.reset(); loB.reset(); restA.reset(); restB.reset(); loAp.reset();
    midA.reset(); midB.reset(); hiA.reset(); hiB.reset();
    for (int b = 0; b < kNumBands; ++b)
    {
        env[b] = 0.0;
        fast[b] = 0.0;
        gain[b] = 1.0;
        target[b] = 1.0;
        step[b] = 0.0;
    }
    wet = 0.0;
    countdown = 0;
}

float OttState::process (float in, double depth) noexcept
{
    const float lo = loAp.process (loB.process (loA.process (in)));
    const float rest = restB.process (restA.process (in));
    const float mid = midB.process (midA.process (rest));
    const float hi = hiB.process (hiA.process (rest));
    const float band[kNumBands] = { lo, mid, hi };

    for (int b = 0; b < kNumBands; ++b)
    {
        const double a = band[b] < 0.0f ? -static_cast<double> (band[b])
                                        : static_cast<double> (band[b]);
        env[b] += (a > env[b] ? atk : rel) * (a - env[b]);
        fast[b] = a > fast[b] ? a : fast[b] + rel * (a - fast[b]);
    }

    if (--countdown <= 0)
    {
        countdown = kInterval;
        for (int b = 0; b < kNumBands; ++b)
        {
            gain[b] = target[b];
            double t = 1.0;
            if (depth > 0.0)
            {
                double g = depth * ottGainDb (20.0 * std::log10 (env[b] + 1e-9));
                // boost (upward + makeup) may lift a band's fast peak only up
                // to kBoostCeilDb: quiet material keeps its full lift, a loud
                // onset the slow follower hasn't caught yet passes at unity
                if (g > 0.0)
                {
                    double cap = kBoostCeilDb - 20.0 * std::log10 (fast[b] + 1e-9);
                    cap = cap < 0.0 ? 0.0 : cap;
                    g = g < cap ? g : cap;
                }
                t = std::pow (10.0, g / 20.0);
            }
            target[b] = t;
            step[b] = (t - gain[b]) / kInterval;
        }
    }

    // Per sample: a boosted band sample may not pass the ceiling either. In
    // steady state the cap above already holds; this catches an onset that
    // arrives before the next tick, while the gain still carries the boost
    // from the quiet tail before it (it drops at once, never below unity).
    double sum = 0.0;
    for (int b = 0; b < kNumBands; ++b)
    {
        const double x = static_cast<double> (band[b]);
        const double a = x < 0.0 ? -x : x;
        if (gain[b] > 1.0 && a * gain[b] > kBoostCeil)
        {
            gain[b] = a > kBoostCeil ? 1.0 : kBoostCeil / a;
            target[b] = gain[b];
            step[b] = 0.0;
        }
        sum += gain[b] * x;
        gain[b] += step[b];
    }

    // Depth 0 is the dry input itself; leaving or reaching it crossfades
    // over 10 ms (dry and the allpassed bands differ in phase).
    if (depth > 0.0)
        wet = wet + wetStep < 1.0 ? wet + wetStep : 1.0;
    else
        wet = wet - wetStep > 0.0 ? wet - wetStep : 0.0;
    if (wet <= 0.0)
        return in;
    const double dry = static_cast<double> (in);
    return static_cast<float> (dry + wet * (sum - dry));
}

} // namespace vibesynth
