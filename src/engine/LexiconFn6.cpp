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

namespace {

// One step of the 4-line FDN (Hadamard feedback matrix, per-line damping
// one-poles). Exactly one call per output sample; L and R are taken from
// two different combinations of the 4 line outputs so both channels carry
// the same tail, decorrelated.
void fdnProcess (MasterRack* rack, float in, double sizeIn,
                 float& wetL, float& wetR) noexcept
{
    auto& rv = rack->reverb;
    const double size = clamp01 (sizeIn);
    const float fb = static_cast<float> (0.35 + 0.50 * size); // ≤ 0.85
    const float scale = static_cast<float> ((0.75 + 0.50 * size)
                                            * rack->sampleRate / 48000.0);
    const float lp = 0.15f + static_cast<float> (size) * 0.5f;
    float taps[FdnReverbState::kNumLines];
    for (int l = 0; l < FdnReverbState::kNumLines; ++l)
    {
        if (rv.buf[l] == nullptr || rv.capacity[l] < 8)
        {
            taps[l] = 0.0f;
            continue;
        }
        float len = static_cast<float> (FdnReverbState::kBaseLen[l]) * scale;
        const float maxLen = static_cast<float> (rv.capacity[l] - 2);
        if (len > maxLen) len = maxLen;
        if (len < 4.0f) len = 4.0f;
        rv.delayLen[l] = len;
        const int i0 = static_cast<int> (len);
        const float frac = len - static_cast<float> (i0);
        int r0 = rv.writePos[l] - i0;
        if (r0 < 0) r0 += rv.capacity[l];
        int r1 = r0 - 1;
        if (r1 < 0) r1 += rv.capacity[l];
        const float tap = rv.buf[l][r0] + frac * (rv.buf[l][r1] - rv.buf[l][r0]);
        rv.lpState[l] += lp * (tap - rv.lpState[l]);
        taps[l] = rv.lpState[l];
    }
    const float m0 = taps[0] + taps[1] + taps[2] + taps[3];
    const float m1 = taps[0] - taps[1] + taps[2] - taps[3];
    const float m2 = taps[0] + taps[1] - taps[2] - taps[3];
    const float m3 = taps[0] - taps[1] - taps[2] + taps[3];
    const float feed[4] = { m0 * 0.5f * fb, m1 * 0.5f * fb,
                            m2 * 0.5f * fb, m3 * 0.5f * fb };
    for (int l = 0; l < FdnReverbState::kNumLines; ++l)
    {
        if (rv.buf[l] == nullptr || rv.capacity[l] < 8)
            continue;
        rv.buf[l][rv.writePos[l]] = sanitizeFloat (in * 0.25f + feed[l]);
        rv.writePos[l] += 1;
        if (rv.writePos[l] >= rv.capacity[l])
            rv.writePos[l] = 0;
    }
    wetL = sanitizeFloat ((taps[0] + taps[2]) * 0.35f);
    wetR = sanitizeFloat ((taps[1] + taps[3]) * 0.35f);
}

} // namespace

EEL_F NSEEL_CGEN_CALL fnBusReverb (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = masterRackOf (ctx);
    if (rack == nullptr || np < 4) return 0.0;
    const float l = sanitizeFloat (static_cast<float> (argD (p, np, 0, 0.0)));
    const float r = sanitizeFloat (static_cast<float> (argD (p, np, 1, 0.0)));
    const double size = argD (p, np, 2, 0.5);
    const float mix = static_cast<float> (clamp01 (argD (p, np, 3, 0.3)));
    float wetL = 0.0f, wetR = 0.0f;
    fdnProcess (rack, (l + r) * 0.5f, size, wetL, wetR);
    const float outL = sanitizeFloat (l * (1.0f - mix) + wetL * mix);
    rack->reverbRight = sanitizeFloat (r * (1.0f - mix) + wetR * mix);
    rack->recordMeter (ModuleId::BusReverb, 0,
                       std::fmax (std::fabs (outL), std::fabs (rack->reverbRight)));
    return static_cast<EEL_F> (outL);
}

EEL_F NSEEL_CGEN_CALL fnBusReverbR (void* ctx, INT_PTR, EEL_F**)
{
    auto* rack = masterRackOf (ctx);
    if (rack == nullptr) return 0.0;
    return static_cast<EEL_F> (rack->reverbRight);
}

} // namespace vibesynth
