// M4 — LexiconFn1.cpp: native wrappers, oscillators + noise.
//
// Each wrapper: fetch VmRackContext via NSEEL_PProc_THIS, clamp args
// (SPEC §5.1), run the module, recordMeter, return the sample.
// Wrappers never allocate, lock, log, throw or do I/O.

#include "engine/LexiconArgs.h"

namespace vibesynth {

// fn* wrappers have external linkage (LexiconReg.cpp registers them);
// file-local cores live in an anonymous namespace.
namespace {

// Shared Daisy oscillator core: waveform is set per call, cache avoids
// re-setting (and recomputing phase increments) when hz is unchanged.
EEL_F oscCore (VoiceRack* rack, daisysp::Oscillator* pool, OscCache* cache,
               ModuleId mod, double slotIn, double hzIn, uint8_t wave,
               double pwIn, bool usePw) noexcept
{
    const int slot = clampSlot (slotIn, kNumOscSlots);
    const double hz = clampHz (hzIn, rack->sampleRate);
    auto& osc = pool[slot];
    auto& c = cache[slot];
    const float hzf = static_cast<float> (hz);
    if (hzf != c.hz)
    {
        osc.SetFreq (hzf);
        c.hz = hzf;
    }
    osc.SetWaveform (wave);
    if (usePw)
    {
        const float pw = static_cast<float> (clamp01 (pwIn) * 0.96 + 0.02);
        if (pw != c.pw)
        {
            osc.SetPw (pw);
            c.pw = pw;
        }
    }
    const float out = sanitizeFloat (osc.Process());
    rack->recordMeter (mod, slot, out);
    return static_cast<EEL_F> (out);
}

} // namespace

EEL_F NSEEL_CGEN_CALL fnOscSine (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 2)
        return 0.0;
    return oscCore (rack, rack->oscSine, rack->oscSineCache, ModuleId::OscSine,
                    argD (p, np, 0, 0.0), argD (p, np, 1, 0.0),
                    daisysp::Oscillator::WAVE_SIN, 0.5, false);
}

EEL_F NSEEL_CGEN_CALL fnOscSaw (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 2)
        return 0.0;
    return oscCore (rack, rack->oscSaw, rack->oscSawCache, ModuleId::OscSaw,
                    argD (p, np, 0, 0.0), argD (p, np, 1, 0.0),
                    daisysp::Oscillator::WAVE_POLYBLEP_SAW, 0.5, false);
}

EEL_F NSEEL_CGEN_CALL fnOscTri (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 2)
        return 0.0;
    return oscCore (rack, rack->oscTri, rack->oscTriCache, ModuleId::OscTri,
                    argD (p, np, 0, 0.0), argD (p, np, 1, 0.0),
                    daisysp::Oscillator::WAVE_POLYBLEP_TRI, 0.5, false);
}

EEL_F NSEEL_CGEN_CALL fnOscSquare (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 3)
        return 0.0;
    return oscCore (rack, rack->oscSquare, rack->oscSquareCache,
                    ModuleId::OscSquare, argD (p, np, 0, 0.0),
                    argD (p, np, 1, 0.0),
                    daisysp::Oscillator::WAVE_POLYBLEP_SQUARE,
                    argD (p, np, 2, 0.5), true);
}

EEL_F NSEEL_CGEN_CALL fnOscSupersaw (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 4)
        return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumSupersawSlots);
    const float out = sanitizeFloat (supersawProcess (rack->supersaw[slot],
        argD (p, np, 1, 0.0), argD (p, np, 2, 0.5), argD (p, np, 3, 0.5),
        rack->sampleRate));
    rack->recordMeter (ModuleId::OscSupersaw, slot, out);
    return static_cast<EEL_F> (out);
}

EEL_F NSEEL_CGEN_CALL fnNoise (void* ctx, INT_PTR, EEL_F**)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr)
        return 0.0;
    return static_cast<EEL_F> (rack->noise.next());
}

} // namespace vibesynth
