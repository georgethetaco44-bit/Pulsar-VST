// Phase 2 — LexiconFn8.cpp: fx_comb_ff and bus_mb (Utilities7.h), method
// bodies and lexicon wrappers.
//
// Real-time rules (SPEC 8.4): the per-sample paths perform no allocation,
// locks, logging, I/O or exceptions; NaN/Inf checks use bit-pattern tests.

#include "engine/LexiconArgs.h"

#include <cmath>

namespace vibesynth {

// ---------------------------------------------------------------------------
// CombFfState

int CombFfState::capacityFor (double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    return static_cast<int> (std::ceil (sr / kMinHz)) + 8;
}

void CombFfState::reset() noexcept
{
    if (buf != nullptr)
        for (int i = 0; i < capacity; ++i)
            buf[i] = 0.0f;
    writePos = 0;
    delay = delay1 = 0.0;
    primed = false;
    cachedHz = -1.0;
    weightsFor = -1.0;
}

float CombFfState::process (float in, double hz, double feedback, double feedforward, double sampleRate) noexcept
{
    if (buf == nullptr || capacity < 16)
        return in;
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    if (! sameBitsD (sr, smoothRate))
    {
        smoothRate = sr;
        smooth = 1.0 - std::exp (-1.0 / (kSmoothSeconds * sr));
        cachedHz = -1.0;
    }
    // The target delay, recomputed only when hz changes.
    if (! sameBitsD (hz, cachedHz))
    {
        cachedHz = hz;
        const double h = (isFiniteBitsD (hz) && hz > kMinHz) ? hz : kMinHz;
        double t = sr / h;
        const double maxDelay = static_cast<double> (capacity - 4);  // the last tap is delay + 3
        t = t > maxDelay ? maxDelay : t;
        target = t < kMinDelay ? kMinDelay : t;
    }
    if (! primed)
    {
        delay1 = delay = target;
        primed = true;
    }
    else if (! (sameBitsD (delay, target) && sameBitsD (delay1, target)))
    {
        delay1 += smooth * (target - delay1);
        delay += smooth * (delay1 - delay);
        // Within a millionth of a sample the glide is over: land on the target exactly.
        if (std::fabs (delay - target) < 1.0e-6 && std::fabs (delay1 - target) < 1.0e-6)
            delay1 = delay = target;
    }

    const float fb = static_cast<float> (! isFiniteBitsD (feedback) ? 0.0 : (feedback < -0.95 ? -0.95 : (feedback > 0.95 ? 0.95 : feedback)));
    const float ff = static_cast<float> (! isFiniteBitsD (feedforward) ? 0.0 : (feedforward < -1.0 ? -1.0 : (feedforward > 1.0 ? 1.0 : feedforward)));

    // Lagrange weights at x = frac, recomputed only when the delay moves.
    // At voice rates (>= 64 kHz) 4 taps (3rd order, d-1 .. d+2) are within
    // 0.02 dB below 8 kHz; at the master's rate 6 taps (5th order, d-2 .. d+3).
    const int d = static_cast<int> (delay);
    const bool six = sr < 64000.0;
    if (! sameBitsD (delay, weightsFor))
    {
        weightsFor = delay;
        const double x = delay - static_cast<double> (d);
        if (six)
        {
            const double a = x + 2.0, b = x + 1.0, c = x, e = x - 1.0, g = x - 2.0, k = x - 3.0;
            const double ab = a * b, abc = ab * c, abce = abc * e;
            const double gk = g * k, egk = e * gk, cegk = c * egk;
            weights[0] = static_cast<float> ((b * cegk) * (-1.0 / 120.0));  // delay d - 2
            weights[1] = static_cast<float> ((a * cegk) * (1.0 / 24.0));    // d - 1
            weights[2] = static_cast<float> ((ab * egk) * (-1.0 / 12.0));   // d
            weights[3] = static_cast<float> ((abc * gk) * (1.0 / 12.0));    // d + 1
            weights[4] = static_cast<float> ((abce * k) * (-1.0 / 24.0));   // d + 2
            weights[5] = static_cast<float> ((abce * g) * (1.0 / 120.0));   // d + 3
        }
        else
        {
            const double b = x + 1.0, e = x - 1.0, g = x - 2.0;
            weights[0] = static_cast<float> (-x * e * g * (1.0 / 6.0));     // delay d - 1
            weights[1] = static_cast<float> (b * e * g * 0.5);              // d
            weights[2] = static_cast<float> (-b * x * g * 0.5);             // d + 1
            weights[3] = static_cast<float> (b * x * e * (1.0 / 6.0));      // d + 2
        }
    }

    // buf[writePos - n] was written n samples ago; summed pairwise (shorter chain).
    const int base = writePos - d;
    const float* wt = weights;
    float delayed = 0.0f;
    if (base - 3 >= 0 && base + 2 < capacity)
    {
        const float* p = buf + base;
        delayed = six ? ((wt[0] * p[2] + wt[1] * p[1]) + (wt[2] * p[0] + wt[3] * p[-1])) + (wt[4] * p[-2] + wt[5] * p[-3])
                      : (wt[0] * p[1] + wt[1] * p[0]) + (wt[2] * p[-1] + wt[3] * p[-2]);
    }
    else
    {
        auto at = [this] (int i) noexcept
        {
            i = i < 0 ? i + capacity : (i >= capacity ? i - capacity : i);
            return buf[i];
        };
        delayed = six ? ((wt[0] * at (base + 2) + wt[1] * at (base + 1)) + (wt[2] * at (base) + wt[3] * at (base - 1)))
                            + (wt[4] * at (base - 2) + wt[5] * at (base - 3))
                      : (wt[0] * at (base + 1) + wt[1] * at (base)) + (wt[2] * at (base - 1) + wt[3] * at (base - 2));
    }

    const float w = in + fb * delayed;
    buf[writePos] = sanitizeFloat (w);
    if (++writePos >= capacity)
        writePos = 0;
    return sanitizeFloat (w + ff * delayed);
}

// ---------------------------------------------------------------------------
// MbCompState

namespace {

enum class MbKind { Lp, Hp, Ap };

// RBJ Butterworth (Q = 1/sqrt 2) biquads. The same bilinear map for all
// three, so LP2^2 + HP2^2 equals the allpass exactly, as in the analogue.
MbCoef mbCoef (MbKind kind, double hz, double sampleRate) noexcept
{
    constexpr double kPi = 3.14159265358979323846;
    const double w0 = 2.0 * kPi * hz / sampleRate;
    const double cw = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * 0.7071067811865476);
    const double a0 = 1.0 + alpha;
    MbCoef q;
    if (kind == MbKind::Lp)
    {
        q.b0 = (1.0 - cw) * 0.5 / a0; q.b1 = (1.0 - cw) / a0; q.b2 = q.b0;
    }
    else if (kind == MbKind::Hp)
    {
        q.b0 = (1.0 + cw) * 0.5 / a0; q.b1 = -(1.0 + cw) / a0; q.b2 = q.b0;
    }
    else
    {
        q.b0 = (1.0 - alpha) / a0; q.b1 = -2.0 * cw / a0; q.b2 = 1.0;
    }
    q.a1 = -2.0 * cw / a0;
    q.a2 = (1.0 - alpha) / a0;
    return q;
}

// One biquad stage on both channels at once (transposed direct form II).
inline void biquad2 (const MbCoef& k, double (&z1)[2], double (&z2)[2], const double (&x)[2], double (&y)[2]) noexcept
{
    for (int c = 0; c < 2; ++c)
    {
        y[c] = k.b0 * x[c] + z1[c];
        z1[c] = k.b1 * x[c] - k.a1 * y[c] + z2[c];
        z2[c] = k.b2 * x[c] - k.a2 * y[c];
    }
}

// 10 log10 (p) = 10 log10 (2) * log2 (p)
constexpr double kDbPerLog2Power = 3.0102999566398120;
// 10^(dB / 20) = 2^(dB * log2 (10) / 20)
constexpr double kLog2PerDbAmp = 0.16609640474436813;
constexpr double kFloorPower = 1.0e-9;  // 10^(kFloorDb / 10)

} // namespace

void MbCompState::setAveraging() noexcept
{
    constexpr double kTwoPi = 6.28318530717958647692;
    const double lowest[kNumBands] = { 30.0, f1, f2 };
    for (int b = 0; b < kNumBands; ++b)
    {
        const double tau = std::fmax (0.0005 * attackMs, 1.0 / (kTwoPi * lowest[b]));
        avg[b] = 1.0 - std::exp (-1.0 / (tau * sampleRate));
    }
}

void MbCompState::init (double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    wetStep = 1.0 / (0.010 * sampleRate);
    f1 = f2 = -1.0;
    setCrossovers (120.0, 2500.0);
    attackMs = 10.0;
    atk = 1.0 - std::exp (-1.0 / (0.010 * sampleRate));
    rel = 1.0 - std::exp (-1.0 / (0.100 * sampleRate));
    setAveraging();
    for (int b = 0; b < kNumBands; ++b)
    {
        lowDb[b] = kMinThresholdDb;
        highDb[b] = kMaxThresholdDb;
        upSlope[b] = downSlope[b] = gainDb[b] = 0.0;
    }
    reset();
}

void MbCompState::reset() noexcept
{
    for (int st = 0; st < kNumStages; ++st)
        for (int c = 0; c < 2; ++c)
            z1[st][c] = z2[st][c] = 0.0;
    for (int b = 0; b < kNumBands; ++b)
        for (int c = 0; c < 2; ++c)
        {
            ms1[b][c] = ms2[b][c] = env[b][c] = 0.0;
            gain[b][c] = 1.0;
            step[b][c] = 0.0;
        }
    gainCountdown = 0;
    haveRaw = false;
    readable = false;
    wet = 0.0;
    countdown = 0;
}

void MbCompState::setCrossovers (double newF1, double newF2) noexcept
{
    if (! sameBitsD (newF1, f1))
    {
        f1 = newF1;
        lp1 = mbCoef (MbKind::Lp, f1, sampleRate);
        hp1 = mbCoef (MbKind::Hp, f1, sampleRate);
    }
    if (! sameBitsD (newF2, f2))
    {
        f2 = newF2;
        ap2 = mbCoef (MbKind::Ap, f2, sampleRate);
        lp2 = mbCoef (MbKind::Lp, f2, sampleRate);
        hp2 = mbCoef (MbKind::Hp, f2, sampleRate);
    }
}

void MbCompState::readSettings (void* vm, double baseIn) noexcept
{
    // Range first: rounding a huge value to int would be undefined.
    if (vm == nullptr || ! isFiniteBitsD (baseIn) || baseIn < -0.5 || baseIn > 65536.0)
    {
        readable = false;
        return;
    }
    const int base = static_cast<int> (std::floor (baseIn + 0.5));
    if (base < 0 || base + kNumValues > 65536)
    {
        readable = false;
        return;
    }
    int valid = 0;
    const EEL_F* ram = NSEEL_VM_getramptr_noalloc (static_cast<NSEEL_VMCTX> (vm), static_cast<unsigned int> (base), &valid);
    if (ram == nullptr || valid < kNumValues)
    {
        readable = false;
        return;
    }
    readable = true;
    bool same = haveRaw;
    for (int i = 0; i < kNumValues && same; ++i)
        same = sameBitsD (static_cast<double> (ram[i]), raw[i]);
    if (same)
        return;
    for (int i = 0; i < kNumValues; ++i)
        raw[i] = static_cast<double> (ram[i]);
    haveRaw = true;

    const auto value = [this] (int i, double dflt, double lo, double hi) noexcept
    {
        const double v = raw[i];
        if (! isFiniteBitsD (v))
            return dflt;
        return v < lo ? lo : (v > hi ? hi : v);
    };
    const double maxX = std::fmin (kMaxXoverHz, 0.45 * sampleRate);
    const double nf1 = value (0, 120.0, kMinXoverHz, maxX);
    double nf2 = value (1, 2500.0, kMinXoverHz, maxX);
    if (nf2 < nf1)
        nf2 = nf1;
    setCrossovers (nf1, nf2);
    const double atkMs = value (2, 10.0, kMinAttackMs, kMaxAttackMs);
    const double relMs = value (3, 100.0, kMinReleaseMs, kMaxReleaseMs);
    atk = 1.0 - std::exp (-1000.0 / (atkMs * sampleRate));
    rel = 1.0 - std::exp (-1000.0 / (relMs * sampleRate));
    attackMs = atkMs;
    setAveraging();
    for (int b = 0; b < kNumBands; ++b)
    {
        const int at = 4 + 5 * b;
        lowDb[b] = value (at, kMinThresholdDb, kMinThresholdDb, kMaxThresholdDb);
        highDb[b] = value (at + 1, kMaxThresholdDb, kMinThresholdDb, kMaxThresholdDb);
        upSlope[b] = 1.0 - 1.0 / value (at + 2, 1.0, kMinRatio, kMaxRatio);
        downSlope[b] = 1.0 - 1.0 / value (at + 3, 1.0, kMinRatio, kMaxRatio);
        gainDb[b] = value (at + 4, 0.0, kMinGainDb, kMaxGainDb);
    }
}

double MbCompState::curveDb (int band, double levelDb) const noexcept
{
    if (band < 0 || band >= kNumBands)
        return 0.0;
    const double l = levelDb > kFloorDb ? levelDb : kFloorDb;
    double g = gainDb[band];
    if (l < lowDb[band])
        g += (lowDb[band] - l) * upSlope[band];
    if (l > highDb[band])
        g -= (l - highDb[band]) * downSlope[band];
    return g;
}

void MbCompState::process (void* vm, float inL, float inR, double base, double mix, float& outL, float& outR) noexcept
{
    if (--countdown <= 0)
    {
        countdown = kInterval;
        readSettings (vm, base);
    }
    const double m = clamp01 (mix);
    const bool on = readable && m > 0.0;

    const double in[2] = { static_cast<double> (inL), static_cast<double> (inR) };
    double t1[2], t2[2], rest[2], band[kNumBands][2];
    biquad2 (lp1, z1[kLo1], z2[kLo1], in, t1);
    biquad2 (lp1, z1[kLo2], z2[kLo2], t1, t2);
    biquad2 (ap2, z1[kLoAp], z2[kLoAp], t2, band[0]);
    biquad2 (hp1, z1[kRest1], z2[kRest1], in, t1);
    biquad2 (hp1, z1[kRest2], z2[kRest2], t1, rest);
    biquad2 (lp2, z1[kMid1], z2[kMid1], rest, t1);
    biquad2 (lp2, z1[kMid2], z2[kMid2], t1, band[1]);
    biquad2 (hp2, z1[kHi1], z2[kHi1], rest, t1);
    biquad2 (hp2, z1[kHi2], z2[kHi2], t1, band[2]);

    for (int b = 0; b < kNumBands; ++b)
        for (int c = 0; c < 2; ++c)
        {
            ms1[b][c] += avg[b] * (band[b][c] * band[b][c] - ms1[b][c]);
            ms2[b][c] += avg[b] * (ms1[b][c] - ms2[b][c]);
            const double ms = ms2[b][c];
            env[b][c] += (ms > env[b][c] ? atk : rel) * (ms - env[b][c]);
        }

    // The gain curve every kGainInterval samples; the linear gain ramps to it.
    if (--gainCountdown <= 0)
    {
        gainCountdown = kGainInterval;
        for (int b = 0; b < kNumBands; ++b)
            for (int c = 0; c < 2; ++c)
            {
                double p = 2.0 * env[b][c];
                p = p > kFloorPower ? p : kFloorPower;
                const double level = kDbPerLog2Power * mbLog2 (p);
                double g = gainDb[b];
                if (level < lowDb[b])
                    g += (lowDb[b] - level) * upSlope[b];
                if (level > highDb[b])
                    g -= (level - highDb[b]) * downSlope[b];
                const double target = mbExp2 (g * kLog2PerDbAmp);
                step[b][c] = (target - gain[b][c]) * (1.0 / kGainInterval);
            }
    }

    double out[2] = {};
    for (int c = 0; c < 2; ++c)
    {
        double sum = 0.0;
        for (int b = 0; b < kNumBands; ++b)
        {
            gain[b][c] += step[b][c];
            sum += band[b][c] * (1.0 + m * (gain[b][c] - 1.0));
        }
        out[c] = sum;
    }

    // Leaving or reaching dry crossfades over 10 ms (the bands are an
    // allpassed copy of the input, so a hard switch would click).
    if (on)
        wet = wet + wetStep < 1.0 ? wet + wetStep : 1.0;
    else
        wet = wet - wetStep > 0.0 ? wet - wetStep : 0.0;
    if (wet <= 0.0)
    {
        outL = inL;
        outR = inR;
        return;
    }
    outL = sanitizeFloat (static_cast<float> (in[0] + wet * (out[0] - in[0])));
    outR = sanitizeFloat (static_cast<float> (in[1] + wet * (out[1] - in[1])));
}

// ---------------------------------------------------------------------------
// Wrappers

EEL_F NSEEL_CGEN_CALL fnFxCombFf (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 5) return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumCombFfSlots);
    const float out = sanitizeFloat (rack->combFf[slot].process (
        sanitizeFloat (static_cast<float> (argD (p, np, 1, 0.0))),
        argD (p, np, 2, 440.0), argD (p, np, 3, 0.0), argD (p, np, 4, 0.0), rack->sampleRate));
    rack->recordMeter (ModuleId::FxCombFf, slot, out);
    return static_cast<EEL_F> (out);
}

EEL_F NSEEL_CGEN_CALL fnBusMb (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = masterRackOf (ctx);
    if (rack == nullptr || np < 4) return 0.0;
    auto* rc = static_cast<VmRackContext*> (ctx);
    const float l = sanitizeFloat (static_cast<float> (argD (p, np, 0, 0.0)));
    const float r = sanitizeFloat (static_cast<float> (argD (p, np, 1, 0.0)));
    float outL = l, outR = r;
    rack->mb.process (rc->vm, l, r, argD (p, np, 2, 0.0), argD (p, np, 3, 1.0), outL, outR);
    rack->mbRight = outR;
    rack->recordMeter (ModuleId::BusMb, 0, std::fmax (std::fabs (outL), std::fabs (outR)));
    return static_cast<EEL_F> (outL);
}

EEL_F NSEEL_CGEN_CALL fnBusMbR (void* ctx, INT_PTR, EEL_F**)
{
    auto* rack = masterRackOf (ctx);
    if (rack == nullptr) return 0.0;
    return static_cast<EEL_F> (rack->mbRight);
}

} // namespace vibesynth
