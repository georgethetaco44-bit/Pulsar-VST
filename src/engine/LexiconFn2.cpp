// M4 — LexiconFn2.cpp: filters + envelopes.
//
// SPEC §4.2.1 wrapper rules: mappings are cached on their raw 0–1 inputs
// and the coefficient-heavy Daisy setters run at control rate (every
// kControlInterval samples per slot; see the caches in Lexicon.h). M7: the
// filters are our own and smooth/map per sample (engine/Filters.h).

#include "engine/LexiconArgs.h"

namespace vibesynth {

namespace {

// Shared SVF core (M7: own TPT SVF, engine/Filters.h). Cutoff and
// resonance are smoothed and mapped per sample inside SvfState.
EEL_F svfCore (VoiceRack* rack, SvfState* pool, ModuleId mod, double slotIn, double in,
               double cutoffIn, double resIn, int tap) noexcept
{
    const int slot = clampSlot (slotIn, kNumSvfSlots);
    const float out = sanitizeFloat (pool[slot].process (sanitizeFloat (static_cast<float> (in)),
                                                         cutoffIn, resIn, rack->filterRate, tap));
    rack->recordMeter (mod, slot, out);
    return static_cast<EEL_F> (out);
}

} // namespace

EEL_F NSEEL_CGEN_CALL fnFltLp (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 4) return 0.0;
    return svfCore (rack, rack->fltLp, ModuleId::FltLp,
                    argD (p, np, 0, 0.0), argD (p, np, 1, 0.0),
                    argD (p, np, 2, 0.5), argD (p, np, 3, 0.0), SvfState::Low);
}

EEL_F NSEEL_CGEN_CALL fnFltHp (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 4) return 0.0;
    return svfCore (rack, rack->fltHp, ModuleId::FltHp,
                    argD (p, np, 0, 0.0), argD (p, np, 1, 0.0),
                    argD (p, np, 2, 0.5), argD (p, np, 3, 0.0), SvfState::High);
}

EEL_F NSEEL_CGEN_CALL fnFltBp (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 4) return 0.0;
    return svfCore (rack, rack->fltBp, ModuleId::FltBp,
                    argD (p, np, 0, 0.0), argD (p, np, 1, 0.0),
                    argD (p, np, 2, 0.5), argD (p, np, 3, 0.0), SvfState::Band);
}

// Ladder (M7: own 4-pole ZDF ladder at the voice rate, engine/Filters.h).
EEL_F NSEEL_CGEN_CALL fnFltLadder (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 4) return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumLadderSlots);
    const float out = sanitizeFloat (rack->fltLadder[slot].process (
        sanitizeFloat (static_cast<float> (argD (p, np, 1, 0.0))),
        argD (p, np, 2, 0.5), argD (p, np, 3, 0.0), rack->filterRate));
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
