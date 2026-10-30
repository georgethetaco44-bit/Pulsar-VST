// M4 — LexiconFn3.cpp: fx_fold/sat/crush/comb/ott + phys_* voice functions.

#include "engine/LexiconArgs.h"

#include <cmath>

namespace vibesynth {

EEL_F NSEEL_CGEN_CALL fnFxFold (void* ctx, INT_PTR np, EEL_F** p)
{
    if (voiceRackOf (ctx) == nullptr || np < 2) return 0.0;
    return static_cast<EEL_F> (foldSample (
        sanitizeFloat (static_cast<float> (argD (p, np, 0, 0.0))),
        argD (p, np, 1, 0.0)));
}

EEL_F NSEEL_CGEN_CALL fnFxSat (void* ctx, INT_PTR np, EEL_F** p)
{
    if (voiceRackOf (ctx) == nullptr || np < 2) return 0.0;
    return static_cast<EEL_F> (satSample (
        sanitizeFloat (static_cast<float> (argD (p, np, 0, 0.0))),
        argD (p, np, 1, 0.0)));
}

EEL_F NSEEL_CGEN_CALL fnFxCrush (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 4) return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumCrushSlots);
    const float bits = static_cast<float> (clamp01 (argD (p, np, 2, 0.0)));
    const float rate = static_cast<float> (clamp01 (argD (p, np, 3, 1.0)));
    // Daisy converts to int32 via x·65536: keep |x| < 32768 (no UB).
    float in = sanitizeFloat (static_cast<float> (argD (p, np, 1, 0.0)));
    if (in > 32767.0f) in = 32767.0f; else if (in < -32767.0f) in = -32767.0f;

    float out = in;
    if (bits > 0.0f || rate < 1.0f)
    {
        auto& c = rack->crush[slot];
        auto& cc = rack->crushCache[slot];
        if (bits != cc.bits)
        {
            // bits 0–1 → 0–14 of Daisy's 16 bits removed (non-smooth mode).
            // bits = 0 leaves only Daisy's 16.16 fixed-point truncation
            // (< -96 dB), i.e. no audible bit reduction.
            c.SetBitcrushFactor (bits * 0.875f);
            cc.bits = bits;
        }
        if (rate != cc.rate)
        {
            // Daisy (Effects/decimator.cpp): threshold = (uint32)(f²·96) and
            // the held sample refreshes every threshold + 1 samples, so f is
            // a hold length in samples, independent of the sample rate.
            // Map rate 1 → hold 1 (passthrough) … rate 0 → hold sr/1000
            // (1 kHz effective rate: heavy but still pitched), exponential.
            const double hold = std::pow (rack->sampleRate / 1000.0,
                                          1.0 - static_cast<double> (rate));
            int holdN = static_cast<int> (hold + 0.5);
            if (holdN < 1) holdN = 1;
            // +0.25 keeps the float product above the integer boundary.
            const double f = std::sqrt ((static_cast<double> (holdN - 1) + 0.25) / 96.0);
            c.SetDownsampleFactor (static_cast<float> (f));
            cc.rate = rate;
        }
        out = sanitizeFloat (c.Process (in));
    }
    rack->recordMeter (ModuleId::FxCrush, slot, out);
    return static_cast<EEL_F> (out);
}

EEL_F NSEEL_CGEN_CALL fnFxComb (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 4) return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumCombSlots);
    const float out = sanitizeFloat (rack->comb[slot].process (
        sanitizeFloat (static_cast<float> (argD (p, np, 1, 0.0))),
        argD (p, np, 2, 440.0), clamp01 (argD (p, np, 3, 0.0)),
        rack->sampleRate));
    rack->recordMeter (ModuleId::FxComb, slot, out);
    return static_cast<EEL_F> (out);
}

// fx_ott(slot, in, depth): 3-band upward + downward compressor (M6).
EEL_F NSEEL_CGEN_CALL fnFxOtt (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 3) return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumOttSlots);
    const float out = sanitizeFloat (rack->ott[slot].process (
        sanitizeFloat (static_cast<float> (argD (p, np, 1, 0.0))),
        clamp01 (argD (p, np, 2, 0.0))));
    rack->recordMeter (ModuleId::FxOtt, slot, out);
    return static_cast<EEL_F> (out);
}

// Physical models: parameters are applied at control ticks only.
EEL_F NSEEL_CGEN_CALL fnPhysResonator (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 6) return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumResonatorSlots);
    auto& r = rack->resonator[slot];
    auto& c = rack->resonCache[slot];
    if (--c.countdown <= 0)
    {
        c.countdown = kControlInterval;
        const float hz = static_cast<float> (clampHz (argD (p, np, 2, 440.0),
                                                      rack->sampleRate));
        const float st = static_cast<float> (clamp01 (argD (p, np, 3, 0.5)));
        const float br = static_cast<float> (clamp01 (argD (p, np, 4, 0.5)));
        const float da = static_cast<float> (clamp01 (argD (p, np, 5, 0.5)));
        if (hz != c.hz) { r.SetFreq (hz); c.hz = hz; }
        if (st != c.st) { r.SetStructure (st); c.st = st; }
        if (br != c.br) { r.SetBrightness (br); c.br = br; }
        if (da != c.da) { r.SetDamping (da); c.da = da; }
    }
    const float out = sanitizeFloat (r.Process (sanitizeFloat (
        static_cast<float> (argD (p, np, 1, 0.0)))));
    rack->recordMeter (ModuleId::PhysResonator, slot, out);
    return static_cast<EEL_F> (out);
}

EEL_F NSEEL_CGEN_CALL fnPhysString (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 5) return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumStringSlots);
    auto& s = rack->string_[slot];
    auto& c = rack->stringCache[slot];
    if (--c.countdown <= 0)
    {
        c.countdown = kControlInterval;
        // Delay line is 1024 samples: clamp to a usable range.
        double hz = clampHz (argD (p, np, 2, 440.0), rack->sampleRate);
        const double lo = rack->sampleRate / 1020.0;
        if (hz < lo) hz = lo;
        const float hzf = static_cast<float> (hz);
        const float br = static_cast<float> (clamp01 (argD (p, np, 3, 0.5)));
        const float da = static_cast<float> (clamp01 (argD (p, np, 4, 0.5)));
        if (hzf != c.hz) { s.SetFreq (hzf); c.hz = hzf; }
        if (br != c.br) { s.SetBrightness (br); c.br = br; }
        if (da != c.da) { s.SetDamping (da); c.da = da; }
    }
    const float out = sanitizeFloat (s.Process (sanitizeFloat (
        static_cast<float> (argD (p, np, 1, 0.0)))));
    rack->recordMeter (ModuleId::PhysString, slot, out);
    return static_cast<EEL_F> (out);
}

} // namespace vibesynth
