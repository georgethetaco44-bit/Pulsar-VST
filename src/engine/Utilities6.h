// M12 Tier 2 — Utilities.h part 6 (SPEC §5.3 Tier 2): fx_phaser and
// fx_fshift (voice racks), bus_delay and bus_width (master rack).
//
// Header-only, allocation-free, audio-thread safe (SPEC §11). The delay's
// buffer is allocated by its owner (EelMaster, off the audio thread) and
// only carved up by init().
#pragma once

#include <cmath>

#include "engine/Filters.h"
#include "engine/Utilities.h"

namespace vibesynth {

constexpr int kNumPhaserSlots = 2;
constexpr int kNumFshiftSlots = 1;

// TPT one-pole gain G = g / (1 + g), g = tan(pi hz / sr), hz clamped to
// 0.45 x sr. Off the hot path (init, or when a raw input changes).
inline double onePoleGain (double hz, double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double f = hz < 1.0 ? 1.0 : (hz > 0.45 * sr ? 0.45 * sr : hz);
    const double g = std::tan (3.14159265358979323846 * f / sr);
    return g / (1.0 + g);
}

// ---------------------------------------------------------------------------
// fx_phaser(slot, in, pos, depth, feedback): 8 first-order allpass stages
// at one break frequency (bilinear, prewarped: FilterRate's table). `pos` (smoothed per sample like the SVF
// cutoff) sweeps it exponentially over 100 Hz – 5 kHz, which puts the four
// notches between ~20 Hz and ~25 kHz; the code moves `pos` (an LFO, an
// envelope, a macro). `depth` mixes the chain in: 0 = dry (bit for bit),
// 1 = equal dry and wet (deepest notches). `feedback` 0–1 feeds the chain
// output back into its input with gain 0–0.85 (sharper peaks); the
// allpass chain has unit gain, so the loop stays stable.
struct PhaserState
{
    static constexpr int kStages = 8;
    // Normalized cutoff (FilterRate's 20 Hz – 20 kHz map) of 100 Hz and
    // 5 kHz: log2(5) / log2(1000) and log2(250) / log2(1000).
    static constexpr double kLoNorm = 0.23299000144533966;
    static constexpr double kSpanNorm = 0.79932500433601898 - kLoNorm;
    static constexpr double kMaxFeedback = 0.85;

    double x1[kStages] = {};  // each stage's previous input
    double y1[kStages] = {};  // and previous output
    double pos = -1.0;        // smoothed (-1: unset)
    double cachePos = -2.0;
    double a = 0.0;           // allpass coefficient (g - 1) / (g + 1) = 2 G - 1
    double last = 0.0;        // the chain output, fed back next sample

    void reset() noexcept { *this = PhaserState(); }

    float process (float in, double posIn, double depthIn, double fbIn, const FilterRate& fr) noexcept
    {
        pos = smoothTowards (pos, clamp01 (posIn), fr.smooth);
        if (! sameBitsD (pos, cachePos))
        {
            cachePos = pos;
            a = 2.0 * fr.ladderG (kLoNorm + kSpanNorm * pos) - 1.0;
        }
        const double depth = clamp01 (depthIn);
        const double fb = kMaxFeedback * clamp01 (fbIn);
        const double x = static_cast<double> (in);
        double y = x + fb * last;
        // Direct form, y = a (x - y1) + x1: the same response as the TPT
        // allpass (2 LP - x) with three dependent operations per stage
        // instead of five (the 8 stages are one serial chain).
        for (int i = 0; i < kStages; ++i)
        {
            const double out = a * (y - y1[i]) + x1[i];
            x1[i] = y;
            y1[i] = out;
            y = out;
        }
        if (! isFiniteBitsD (y))
        {
            reset();
            return 0.0f;
        }
        last = clampSafe (y);
        const double wet = 0.5 * depth;
        return static_cast<float> (clampSafe (x * (1.0 - wet) + y * wet));
    }
};

// ---------------------------------------------------------------------------
// fx_fshift(slot, in, shift): single-sideband frequency shifter. An IIR
// Hilbert pair (two chains of four 2nd-order allpasses, O. Niemitalo's
// public-domain coefficients; ~90 degrees apart over ~15 Hz – 0.5 x sr at
// 44.1 kHz, the low edge scaling with the rate) gives I and Q; a rotating
// phasor at the shift frequency mixes them: out = I cos + Q sin moves every
// partial up by the shift (down for a negative one; the smoketest checks
// the direction and the other sideband). `shift` 0.5 = none;
// toward 0/1 the shift grows exponentially to -/+2 kHz:
//   hz = sign(s) 2000 (2^(10|s|) - 1) / 1023,  s = 2 shift - 1
// (|s| 0.1 = 2 Hz, 0.5 = 61 Hz, 0.8 = 498 Hz). The phasor's step is
// recomputed only when the raw `shift` changes and renormalized every
// sample. At 0.5 the output is the I chain: the input's own spectrum,
// phase-shifted (an allpass), not the input bit for bit.
struct FshiftState
{
    static constexpr int kSections = 4;
    static constexpr double kA[kSections] = { 0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737 };
    static constexpr double kB[kSections] = { 0.4021921162426, 0.8561710882420, 0.9722909545651,
                                              0.9952884791278 };
    static constexpr double kMaxHz = 2000.0;

    struct Section
    {
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        double run (double x, double a2) noexcept
        {
            const double y = a2 * (x + y2) - x2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            return y;
        }
    };

    Section a[kSections];
    Section b[kSections];
    double aDelayed = 0.0;   // the A chain is one sample behind B
    double re = 1.0, im = 0.0;
    double rawShift = -1.0;  // cache key (-1: unset)
    double rotC = 1.0, rotS = 0.0;

    void reset() noexcept { *this = FshiftState(); }

    static double shiftHz (double shift) noexcept
    {
        const double s = 2.0 * clamp01 (shift) - 1.0;
        const double mag = kMaxHz * (std::exp2 (10.0 * std::fabs (s)) - 1.0) / 1023.0;
        return s < 0.0 ? -mag : mag;
    }

    float process (float in, double shiftIn, double sampleRate) noexcept
    {
        const double shift = isFiniteBitsD (shiftIn) ? shiftIn : 0.5;
        if (! sameBitsD (shift, rawShift))
        {
            rawShift = shift;
            const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
            double hz = shiftHz (shift);
            const double hi = 0.45 * sr;
            hz = hz > hi ? hi : (hz < -hi ? -hi : hz);
            const double w = 2.0 * 3.14159265358979323846 * hz / sr;
            rotC = std::cos (w);
            rotS = std::sin (w);
        }
        const double x = static_cast<double> (in);
        double ya = x, yb = x;
        for (int i = 0; i < kSections; ++i)
        {
            ya = a[i].run (ya, kA[i] * kA[i]);
            yb = b[i].run (yb, kB[i] * kB[i]);
        }
        const double I = aDelayed;
        aDelayed = ya;
        const double Q = yb;
        const double out = I * re + Q * im;
        const double nre = re * rotC - im * rotS;
        const double nim = re * rotS + im * rotC;
        const double k = 1.5 - 0.5 * (nre * nre + nim * nim);
        re = nre * k;
        im = nim * k;
        if (! isFiniteBitsD (out) || ! isFiniteBitsD (aDelayed) || ! isFiniteBitsD (yb) || ! isFiniteBitsD (re)
            || ! isFiniteBitsD (im))
        {
            const double keepRaw = rawShift, keepC = rotC, keepS = rotS;
            reset();
            rawShift = keepRaw;
            rotC = keepC;
            rotS = keepS;
            return 0.0f;
        }
        return static_cast<float> (clampSafe (out));
    }
};

// ---------------------------------------------------------------------------
// bus_delay(l, r, beats, feedback, mix): ping-pong delay synced to the host
// tempo. The input's mono sum enters the left line; the left line feeds the
// right, the right feeds back into the left, so echoes alternate L, R, L...
// each `feedback` (0–1 -> 0–0.9) quieter than the last. Every line input
// passes a 150 Hz low-cut (two one-poles) and a 6 kHz high-cut (one
// one-pole), so the echoes darken as they repeat and the sub never enters
// the wet signal; the filters' gain is <= 1, so the loop gain stays < 1.
// `mix` crossfades dry to wet like bus_reverb (0 = dry, bit for bit).
//
// Time: `beats` (1/16 – 4) x 60 / bpm, capped to the buffer (4 beats at
// 40 bpm). The delay glides to a new time (tempo change, a moving `beats`)
// with a ~50 ms one-pole, its slope capped at 0.1 sample per sample (a
// tape-like pitch bend of at most ~10 %), read with linear interpolation,
// so it never clicks. A reset is O(1): reads older than what was written
// since the reset return 0, so the guard never clears seconds of buffer on
// the audio thread.
struct DelayBusState
{
    static constexpr double kMaxBeats = 4.0;
    static constexpr double kMinBeats = 0.0625;
    static constexpr double kMinBpm = 40.0;
    static constexpr double kMaxFeedback = 0.9;
    static constexpr double kMaxSlew = 0.1;

    // Frames the buffer needs at `sampleRate` (the owner allocates 2x this).
    static int framesFor (double sampleRate) noexcept
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        return static_cast<int> (std::ceil (kMaxBeats * 60.0 / kMinBpm * sr)) + 8;
    }

    float* bufL = nullptr;
    float* bufR = nullptr;
    int capacity = 0;
    int writePos = 0;
    int filled = 0;
    double sampleRate = 48000.0;
    double delay = -1.0;  // current delay in samples (-1: unset, snaps)
    double glide = 1.0;
    double hpG = 0.0, lpG = 0.0;
    double hpL1 = 0.0, hpL2 = 0.0, lpL = 0.0;
    double hpR1 = 0.0, hpR2 = 0.0, lpR = 0.0;

    void init (double sr, float* storage, int frames) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 48000.0;
        const bool ok = storage != nullptr && frames >= 16;
        bufL = ok ? storage : nullptr;
        bufR = ok ? storage + frames : nullptr;
        capacity = ok ? frames : 0;
        glide = 1.0 - std::exp (-1.0 / (0.05 * sampleRate));
        hpG = onePoleGain (150.0, sampleRate);
        lpG = onePoleGain (6000.0, sampleRate);
        reset();
    }

    void reset() noexcept
    {
        writePos = 0;
        filled = 0;
        delay = -1.0;
        hpL1 = hpL2 = lpL = hpR1 = hpR2 = lpR = 0.0;
    }

    static double onePoleLp (double x, double& s, double G) noexcept
    {
        const double v = (x - s) * G;
        const double lp = v + s;
        s = lp + v;
        return lp;
    }

    double read (const float* buf, double d) const noexcept
    {
        const int i0 = static_cast<int> (d);
        if (i0 + 1 > filled)
            return 0.0;
        const double frac = d - static_cast<double> (i0);
        int r0 = writePos - i0;
        if (r0 < 0)
            r0 += capacity;
        int r1 = r0 - 1;
        if (r1 < 0)
            r1 += capacity;
        const double a = static_cast<double> (buf[r0]);
        return a + frac * (static_cast<double> (buf[r1]) - a);
    }

    // Low-cut (two one-poles) then high-cut (one one-pole).
    double filter (double x, double& h1, double& h2, double& lp) const noexcept
    {
        const double a = x - onePoleLp (x, h1, hpG);
        const double b = a - onePoleLp (a, h2, hpG);
        return onePoleLp (b, lp, lpG);
    }

    void process (float l, float r, double beatsIn, double fbIn, double mixIn, double bpmIn, float& outL,
                  float& outR) noexcept
    {
        const double mix = clamp01 (mixIn);
        if (bufL == nullptr || capacity < 16)
        {
            outL = static_cast<float> (static_cast<double> (l) * (1.0 - mix));
            outR = static_cast<float> (static_cast<double> (r) * (1.0 - mix));
            return;
        }
        double beats = isFiniteBitsD (beatsIn) ? beatsIn : 1.0;
        beats = beats < kMinBeats ? kMinBeats : (beats > kMaxBeats ? kMaxBeats : beats);
        double bpm = isFiniteBitsD (bpmIn) ? bpmIn : 120.0;
        bpm = bpm < 20.0 ? 20.0 : (bpm > 999.0 ? 999.0 : bpm);
        double target = beats * 60.0 / bpm * sampleRate;
        const double maxD = static_cast<double> (capacity - 2);
        target = target < 1.0 ? 1.0 : (target > maxD ? maxD : target);
        if (delay < 0.0)
            delay = target;
        else
        {
            const double d = target - delay;
            if (d < 1.0e-6 && d > -1.0e-6)
                delay = target;
            else
            {
                double step = d * glide;
                step = step > kMaxSlew ? kMaxSlew : (step < -kMaxSlew ? -kMaxSlew : step);
                delay += step;
            }
        }

        const double fb = kMaxFeedback * clamp01 (fbIn);
        const double tapL = read (bufL, delay);
        const double tapR = read (bufR, delay);
        const double mono = 0.5 * (static_cast<double> (l) + static_cast<double> (r));
        double wL = filter (mono + fb * tapR, hpL1, hpL2, lpL);
        double wR = filter (fb * tapL, hpR1, hpR2, lpR);
        if (! isFiniteBitsD (wL) || ! isFiniteBitsD (wR))
        {
            hpL1 = hpL2 = lpL = hpR1 = hpR2 = lpR = 0.0;
            wL = wR = 0.0;
        }
        bufL[writePos] = static_cast<float> (clampSafe (wL));
        bufR[writePos] = static_cast<float> (clampSafe (wR));
        if (++writePos >= capacity)
            writePos = 0;
        if (filled < capacity)
            ++filled;

        outL = sanitizeFloat (static_cast<float> (static_cast<double> (l) * (1.0 - mix) + tapL * mix));
        outR = sanitizeFloat (static_cast<float> (static_cast<double> (r) * (1.0 - mix) + tapR * mix));
    }
};

// ---------------------------------------------------------------------------
// bus_width(l, r, amount): mid/side width above ~150 Hz. The side signal
// (L - R) / 2 runs through a 150 Hz TPT SVF (Q 0.707). Widening scales its
// high-pass: side' = side + w hp(side), w = 2 amount - 1 (1 = twice the
// side above 150 Hz; the low end's side is left alone). Narrowing removes
// everything but its low-pass: side' = side - w' (side - lp(side)),
// w' = 1 - 2 amount, so 0 = mono above ~150 Hz (-45 dB at 2 kHz; the
// high-pass alone would leave its band-pass part, -20 dB). Both terms are 0
// at 0.5: unchanged, bit for bit. A mono input (L == R) is never changed,
// so a mono sub stays mono at any width.
struct WidthBusState
{
    double a1 = 0.0, a2 = 0.0, a3 = 0.0, k = 1.4142135623730951;
    double ic1 = 0.0, ic2 = 0.0;

    void init (double sampleRate) noexcept
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        const double g = std::tan (3.14159265358979323846 * 150.0 / sr);
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
        reset();
    }

    void reset() noexcept { ic1 = ic2 = 0.0; }

    void process (float l, float r, double amountIn, float& outL, float& outR) noexcept
    {
        const double x = 0.5 * (static_cast<double> (l) - static_cast<double> (r));
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        double hp = x - k * v1 - v2;
        double hi = x - v2;  // everything but the low-pass
        if (! isFiniteBitsD (hp) || ! isFiniteBitsD (hi) || ! isFiniteBitsD (ic1) || ! isFiniteBitsD (ic2))
        {
            reset();
            hp = hi = 0.0;
        }
        const double amount = isFiniteBitsD (amountIn) ? clamp01 (amountIn) : 0.5;
        const double w = 2.0 * amount - 1.0;
        const double extra = w * (w >= 0.0 ? hp : hi);
        outL = sanitizeFloat (static_cast<float> (static_cast<double> (l) + extra));
        outR = sanitizeFloat (static_cast<float> (static_cast<double> (r) - extra));
    }
};

} // namespace vibesynth
