// M4 — LexiconFn6.cpp: bus_chorus + own FDN bus_reverb.

#include "engine/LexiconArgs.h"

#include <cmath>

namespace vibesynth {

EEL_F NSEEL_CGEN_CALL fnBusChorus (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = masterRackOf (ctx);
    if (rack == nullptr || np < 4) return 0.0;
    const float l = sanitizeFloat (static_cast<float> (argD (p, np, 0, 0.0)));
    const float r = sanitizeFloat (static_cast<float> (argD (p, np, 1, 0.0)));
    const float depth = static_cast<float> (clamp01 (argD (p, np, 2, 0.3)));
    const float mix = static_cast<float> (clamp01 (argD (p, np, 3, 0.3)));
    // ChorusEngine delay line is 2400 samples; guard SetDelayMs at any rate.
    const float sr = static_cast<float> (rack->sampleRate);
    const float maxDelayMs = (2400.0f - 8.0f) / sr * 1000.0f;
    float delayMs = 8.0f + depth * (maxDelayMs - 8.0f);
    if (delayMs < 8.0f) delayMs = 8.0f;
    if (delayMs > maxDelayMs) delayMs = maxDelayMs;
    rack->chorusL.SetDelayMs (delayMs);
    rack->chorusR.SetDelayMs (delayMs);
    rack->chorusL.SetLfoDepth (depth * 0.93f);
    rack->chorusR.SetLfoDepth (depth * 0.93f);
    const float wetL = sanitizeFloat (rack->chorusL.Process (l));
    const float wetR = sanitizeFloat (rack->chorusR.Process (r));
    const float outL = sanitizeFloat (l * (1.0f - mix) + wetL * mix);
    rack->chorusRight = sanitizeFloat (r * (1.0f - mix) + wetR * mix);
    rack->recordMeter (ModuleId::BusChorus, 0,
                       std::fmax (std::fabs (outL), std::fabs (rack->chorusRight)));
    return static_cast<EEL_F> (outL);
}

EEL_F NSEEL_CGEN_CALL fnBusChorusR (void* ctx, INT_PTR, EEL_F**)
{
    auto* rack = masterRackOf (ctx);
    if (rack == nullptr) return 0.0;
    return static_cast<EEL_F> (rack->chorusRight);
}

double FdnReverbState::rt60For (double size) noexcept
{
    // 0.3 s at size 0 to 6 s at size 1, exponential: 0.3 * 20^size.
    return 0.3 * std::exp (2.995732273553991 * clamp01 (size));
}

// One step of the 8-line FDN. Each channel passes two allpass diffusers
// and feeds 4 lines (L the even ones, R the odd ones); the lines are mixed
// by an 8x8 Hadamard matrix (orthogonal, so the loop loses energy only
// through the per-line gains and damping). L and R read two orthogonal
// sign patterns of the 8 line outputs: the same tail, decorrelated.
void FdnReverbState::process (float inL, float inR, double sizeIn, double sampleRate,
                              float& wetL, float& wetR) noexcept
{
    const double size = clamp01 (sizeIn);
    // A new size is taken at once unless one was taken in the last
    // kSizeInterval samples; a size held still is never recomputed, and the
    // first call after a reset always takes it.
    if (sizeHold > 0)
        --sizeHold;
    if (! sameBitsD (size, cachedSize) && (sizeHold == 0 || ! primed))
    {
        sizeHold = kSizeInterval;
        // Per-line gain for the decay time: -60 dB after rt60 seconds. The
        // wet level is normalised by the loop's steady-state power gain,
        // 1 / (1 - mean g^2), over 8 lines summed per channel.
        cachedSize = size;
        const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        const double rt = rt60For (size);
        double meanG2 = 0.0;
        for (int l = 0; l < kNumLines; ++l)
        {
            const double g = std::pow (10.0, -3.0 * static_cast<double> (baseLen[l]) / (sr * rt));
            gainTarget[l] = static_cast<float> (g);
            meanG2 += g * g;
        }
        meanG2 /= kNumLines;
        wetScaleTarget = static_cast<float> (std::sqrt ((1.0 - meanG2) / kNumLines));
    }
    // A new size glides over about 5 ms (a jump in the loop gains or the
    // wet scale would click); the first call after a reset starts there.
    if (! primed)
    {
        primed = true;
        for (int l = 0; l < kNumLines; ++l)
            gain[l] = gainTarget[l];
        wetScale = wetScaleTarget;
    }
    else
    {
        for (int l = 0; l < kNumLines; ++l)
            gain[l] += glide * (gainTarget[l] - gain[l]);
        wetScale += glide * (wetScaleTarget - wetScale);
    }

    // The two modulation LFOs: rotate, then pull the length back to 1.
    for (int k = 0; k < 2; ++k)
    {
        const double c = lfoC[k] * rotC[k] - lfoS[k] * rotS[k];
        const double s = lfoS[k] * rotC[k] + lfoC[k] * rotS[k];
        const double fix = 1.5 - 0.5 * (c * c + s * s);
        lfoC[k] = c * fix;
        lfoS[k] = s * fix;
    }
    const float m1s = static_cast<float> (lfoS[0]), m1c = static_cast<float> (lfoC[0]);
    const float m2s = static_cast<float> (lfoS[1]), m2c = static_cast<float> (lfoC[1]);
    const float mod[kNumLines] = { m1s, m1c, -m1s, -m1c, m2s, m2c, -m2s, -m2c };

    // Input diffusion: Schroeder allpasses, g = 0.6.
    float x[2] = { inL, inR };
    for (int d = 0; d < kNumDiffusers; ++d)
    {
        if (diffBuf[d] == nullptr)
            continue;
        float& v = x[d >> 1];
        const float delayed = diffBuf[d][diffPos[d]];
        const float w = v + 0.6f * delayed;
        diffBuf[d][diffPos[d]] = sanitizeFloat (w);
        v = delayed - 0.6f * w;
        if (++diffPos[d] >= diffLen[d])
            diffPos[d] = 0;
    }

    float y[kNumLines];
    float s[kNumLines];
    for (int l = 0; l < kNumLines; ++l)
    {
        if (buf[l] == nullptr)
        {
            y[l] = 0.0f;
            s[l] = 0.0f;
            continue;
        }
        const float len = baseLen[l] + modDepth * mod[l];
        const int i0 = static_cast<int> (len);
        const float frac = len - static_cast<float> (i0);
        int r0 = writePos[l] - i0;
        if (r0 < 0) r0 += capacity[l];
        int r1 = r0 - 1;
        if (r1 < 0) r1 += capacity[l];
        const float tap = buf[l][r0] + frac * (buf[l][r1] - buf[l][r0]);
        lpState[l] += damp * (tap - lpState[l]);
        const float blocked = lpState[l] - dcIn[l] + dcPole * dcOut[l];
        dcIn[l] = lpState[l];
        dcOut[l] = sanitizeFloat (blocked);
        y[l] = dcOut[l];
        s[l] = y[l] * gain[l];
    }

    // Fast Walsh-Hadamard transform, scaled by 1/sqrt(8).
    for (int h = 1; h < kNumLines; h <<= 1)
        for (int i = 0; i < kNumLines; i += h << 1)
            for (int j = i; j < i + h; ++j)
            {
                const float a = s[j];
                const float b = s[j + h];
                s[j] = a + b;
                s[j + h] = a - b;
            }
    constexpr float kNorm = 0.35355339059327373f;
    for (int l = 0; l < kNumLines; ++l)
    {
        if (buf[l] == nullptr)
            continue;
        buf[l][writePos[l]] = sanitizeFloat (s[l] * kNorm + x[l & 1]);
        if (++writePos[l] >= capacity[l])
            writePos[l] = 0;
    }

    const float sumL = y[0] + y[1] + y[2] - y[3] + y[4] + y[5] + y[6] - y[7];
    const float sumR = y[0] + y[1] - y[2] + y[3] + y[4] + y[5] - y[6] + y[7];
    wetL = sanitizeFloat (sumL * wetScale);
    wetR = sanitizeFloat (sumR * wetScale);
}

EEL_F NSEEL_CGEN_CALL fnBusReverb (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = masterRackOf (ctx);
    if (rack == nullptr || np < 4) return 0.0;
    const float l = sanitizeFloat (static_cast<float> (argD (p, np, 0, 0.0)));
    const float r = sanitizeFloat (static_cast<float> (argD (p, np, 1, 0.0)));
    const double size = argD (p, np, 2, 0.5);
    const double mix = clamp01 (argD (p, np, 3, 0.3));
    auto& rv = rack->reverb;
    float wetL = 0.0f, wetR = 0.0f;
    rv.process (l, r, size, rack->sampleRate, wetL, wetR);
    // Equal-power blend; a new mix glides over about 5 ms. Mix 0 is the dry
    // input bit for bit once the wet gain has faded (at once if it starts at 0).
    if (! sameBitsD (mix, rv.cachedMix))
    {
        rv.cachedMix = mix;
        rv.dryTarget = static_cast<float> (std::cos (kHalfPi * mix));
        rv.wetTarget = static_cast<float> (std::sin (kHalfPi * mix));
    }
    if (! rv.mixPrimed)
    {
        rv.mixPrimed = true;
        rv.dryGain = rv.dryTarget;
        rv.wetGain = rv.wetTarget;
    }
    else
    {
        rv.dryGain += rv.glide * (rv.dryTarget - rv.dryGain);
        rv.wetGain += rv.glide * (rv.wetTarget - rv.wetGain);
    }
    float outL = l;
    float outR = r;
    if (mix <= 0.0 && rv.wetGain < 1.0e-4f)
    {
        rv.dryGain = 1.0f;
        rv.wetGain = 0.0f;
    }
    else
    {
        outL = sanitizeFloat (l * rv.dryGain + wetL * rv.wetGain);
        outR = sanitizeFloat (r * rv.dryGain + wetR * rv.wetGain);
    }
    rack->reverbRight = outR;
    rack->recordMeter (ModuleId::BusReverb, 0, std::fmax (std::fabs (outL), std::fabs (outR)));
    return static_cast<EEL_F> (outL);
}

EEL_F NSEEL_CGEN_CALL fnBusReverbR (void* ctx, INT_PTR, EEL_F**)
{
    auto* rack = masterRackOf (ctx);
    if (rack == nullptr) return 0.0;
    return static_cast<EEL_F> (rack->reverbRight);
}

} // namespace vibesynth
