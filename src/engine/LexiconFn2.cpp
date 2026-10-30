// M4 — LexiconFn2.cpp: filters + envelopes.
//
// SPEC §4.2.1 wrapper rules: mappings are cached on their raw 0–1 inputs
// and the coefficient-heavy Daisy setters run at control rate (every
// kControlInterval samples per slot; see the caches in Lexicon.h).

#include "engine/LexiconArgs.h"

namespace vibesynth {

namespace {

// Shared Svf core: Process() updates all outputs; the wrapper picks a tap.
// Cutoff/res are read at control ticks only (Daisy's SetFreq/SetRes cost a
// sinf + powf each), so the cutoff moves in 16-sample steps.
EEL_F svfCore (VoiceRack* rack, daisysp::Svf* pool, SvfCache* cache,
               ModuleId mod, double slotIn, double in,
               double cutoffIn, double resIn, int tap) noexcept
{
    const int slot = clampSlot (slotIn, kNumSvfSlots);
    auto& f = pool[slot];
    auto& c = cache[slot];
    if (--c.countdown <= 0)
    {
        c.countdown = kControlInterval;
        if (cutoffIn != c.rawCut)
        {
            c.rawCut = cutoffIn;
            const float hz = static_cast<float> (cutoffFromNorm (cutoffIn, rack->sampleRate));
            if (hz != c.hz) { f.SetFreq (hz); c.hz = hz; }
        }
        if (resIn != c.rawRes)
        {
            c.rawRes = resIn;
            const float res = static_cast<float> (clamp01 (resIn));
            if (res != c.res) { f.SetRes (res); c.res = res; }
        }
    }
    f.Process (sanitizeFloat (static_cast<float> (in)));
    float out = 0.0f;
    if (tap == 0) out = f.Low();
    else if (tap == 1) out = f.High();
    else out = f.Band();
    out = sanitizeFloat (out);
    rack->recordMeter (mod, slot, out);
    return static_cast<EEL_F> (out);
}

} // namespace

EEL_F NSEEL_CGEN_CALL fnFltLp (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 4) return 0.0;
    return svfCore (rack, rack->fltLp, rack->fltLpCache, ModuleId::FltLp,
                    argD (p, np, 0, 0.0), argD (p, np, 1, 0.0),
                    argD (p, np, 2, 0.5), argD (p, np, 3, 0.0), 0);
}

EEL_F NSEEL_CGEN_CALL fnFltHp (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 4) return 0.0;
    return svfCore (rack, rack->fltHp, rack->fltHpCache, ModuleId::FltHp,
                    argD (p, np, 0, 0.0), argD (p, np, 1, 0.0),
                    argD (p, np, 2, 0.5), argD (p, np, 3, 0.0), 1);
}

EEL_F NSEEL_CGEN_CALL fnFltBp (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 4) return 0.0;
    return svfCore (rack, rack->fltBp, rack->fltBpCache, ModuleId::FltBp,
                    argD (p, np, 0, 0.0), argD (p, np, 1, 0.0),
                    argD (p, np, 2, 0.5), argD (p, np, 3, 0.0), 2);
}

// Ladder: at each control tick the cutoff target is mapped (cached on the
// raw input) and ramped linearly over the next kControlInterval samples.
// Daisy's LadderFilter::SetFreq is a short polynomial (no transcendental),
// so applying the ramp per sample is cheap and a fast sweep can't zipper.
EEL_F NSEEL_CGEN_CALL fnFltLadder (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 4) return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumLadderSlots);
    auto& f = rack->fltLadder[slot];
    auto& c = rack->ladderCache[slot];
    if (--c.countdown <= 0)
    {
        c.countdown = kControlInterval;
        const double rawCut = argD (p, np, 2, 0.5);
        if (rawCut != c.rawCut)
        {
            c.rawCut = rawCut;
            c.target = static_cast<float> (cutoffFromNorm (rawCut, rack->sampleRate));
        }
        if (c.hz < 0.0f)
        {
            c.hz = c.target; // first tick after init/reset: no ramp
            f.SetFreq (c.hz);
            c.rampLeft = 0;
        }
        else if (c.target != c.hz)
        {
            c.step = (c.target - c.hz) / static_cast<float> (kControlInterval);
            c.rampLeft = kControlInterval;
        }
        const double rawRes = argD (p, np, 3, 0.0);
        if (rawRes != c.rawRes)
        {
            c.rawRes = rawRes;
            f.SetRes (static_cast<float> (clamp01 (rawRes) * 1.8));
        }
    }
    if (c.rampLeft > 0)
    {
        --c.rampLeft;
        c.hz = (c.rampLeft == 0) ? c.target : c.hz + c.step;
        f.SetFreq (c.hz);
    }
    const float out = sanitizeFloat (f.Process (sanitizeFloat (
        static_cast<float> (argD (p, np, 1, 0.0)))));
    rack->recordMeter (ModuleId::FltLadder, slot, out);
    return static_cast<EEL_F> (out);
}

// ADSR: times and sustain are applied at control ticks and on every gate
// edge (a new note always starts with its own times); each time is mapped
// only when its raw input changed. The gate itself is read every sample.
EEL_F NSEEL_CGEN_CALL fnEnvAdsr (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 6) return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumAdsrSlots);
    const bool gate = argD (p, np, 1, 0.0) >= 0.5;
    auto& e = rack->envAdsr[slot];
    auto& c = rack->adsrCache[slot];
    if (--c.countdown <= 0 || gate != c.gate)
    {
        c.countdown = kControlInterval;
        const double rawA = argD (p, np, 2, 0.1);
        const double rawD = argD (p, np, 3, 0.1);
        const double rawS = argD (p, np, 4, 0.7);
        const double rawR = argD (p, np, 5, 0.1);
        if (rawA != c.rawA)
        {
            c.rawA = rawA;
            e.SetAttackTime (static_cast<float> (secondsFromNorm (rawA)), 0.0f);
        }
        if (rawD != c.rawD)
        {
            c.rawD = rawD;
            e.SetDecayTime (static_cast<float> (secondsFromNorm (rawD)));
        }
        if (rawS != c.rawS)
        {
            c.rawS = rawS;
            e.SetSustainLevel (static_cast<float> (clamp01 (rawS)));
        }
        if (rawR != c.rawR)
        {
            c.rawR = rawR;
            e.SetReleaseTime (static_cast<float> (secondsFromNorm (rawR)));
        }
    }
    c.gate = gate;
    const float out = sanitizeFloat (e.Process (gate));
    rack->recordMeter (ModuleId::EnvAdsr, slot, out);
    return static_cast<EEL_F> (out);
}

EEL_F NSEEL_CGEN_CALL fnEnvFollow (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 2) return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumEnvFollowSlots);
    const float out = sanitizeFloat (rack->envFollow[slot].process (
        sanitizeFloat (static_cast<float> (argD (p, np, 1, 0.0)))));
    rack->recordMeter (ModuleId::EnvFollow, slot, out);
    return static_cast<EEL_F> (out);
}

} // namespace vibesynth
