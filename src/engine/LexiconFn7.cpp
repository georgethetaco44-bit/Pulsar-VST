// M12 Tier 2 — LexiconFn7.cpp: fx_phaser, fx_fshift, bus_delay, bus_width
// (SPEC §5.3 Tier 2; DSP in Utilities6.h, registered in LexiconReg.cpp).
//
// bus_delay / bus_width return the LEFT result; the RIGHT result is cached
// in the MasterRack and read by the matching _r() call (the bus_chorus /
// bus_reverb pattern).

#include "engine/LexiconArgs.h"

#include <cmath>

namespace vibesynth {

EEL_F NSEEL_CGEN_CALL fnFxPhaser (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 5) return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumPhaserSlots);
    const float out = sanitizeFloat (rack->fxPhaser[slot].process (
        sanitizeFloat (static_cast<float> (argD (p, np, 1, 0.0))),
        argD (p, np, 2, 0.5), argD (p, np, 3, 0.5), argD (p, np, 4, 0.0), rack->filterRate));
    rack->recordMeter (ModuleId::FxPhaser, slot, out);
    return static_cast<EEL_F> (out);
}

EEL_F NSEEL_CGEN_CALL fnFxFshift (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 3) return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumFshiftSlots);
    const float out = sanitizeFloat (rack->fxFshift[slot].process (
        sanitizeFloat (static_cast<float> (argD (p, np, 1, 0.0))), argD (p, np, 2, 0.5), rack->sampleRate));
    rack->recordMeter (ModuleId::FxFshift, slot, out);
    return static_cast<EEL_F> (out);
}

EEL_F NSEEL_CGEN_CALL fnBusDelay (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = masterRackOf (ctx);
    if (rack == nullptr || np < 5) return 0.0;
    const float l = sanitizeFloat (static_cast<float> (argD (p, np, 0, 0.0)));
    const float r = sanitizeFloat (static_cast<float> (argD (p, np, 1, 0.0)));
    float outL = 0.0f, outR = 0.0f;
    rack->delay.process (l, r, argD (p, np, 2, 0.75), argD (p, np, 3, 0.3), argD (p, np, 4, 0.2), rack->bpm,
                         outL, outR);
    rack->delayRight = outR;
    rack->recordMeter (ModuleId::BusDelay, 0, std::fmax (std::fabs (outL), std::fabs (outR)));
    return static_cast<EEL_F> (outL);
}

EEL_F NSEEL_CGEN_CALL fnBusDelayR (void* ctx, INT_PTR, EEL_F**)
{
    auto* rack = masterRackOf (ctx);
    if (rack == nullptr) return 0.0;
    return static_cast<EEL_F> (rack->delayRight);
}

EEL_F NSEEL_CGEN_CALL fnBusWidth (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = masterRackOf (ctx);
    if (rack == nullptr || np < 3) return 0.0;
    const float l = sanitizeFloat (static_cast<float> (argD (p, np, 0, 0.0)));
    const float r = sanitizeFloat (static_cast<float> (argD (p, np, 1, 0.0)));
    float outL = 0.0f, outR = 0.0f;
    rack->width.process (l, r, argD (p, np, 2, 0.5), outL, outR);
    rack->widthRight = outR;
    rack->recordMeter (ModuleId::BusWidth, 0, std::fmax (std::fabs (outL), std::fabs (outR)));
    return static_cast<EEL_F> (outL);
}

EEL_F NSEEL_CGEN_CALL fnBusWidthR (void* ctx, INT_PTR, EEL_F**)
{
    auto* rack = masterRackOf (ctx);
    if (rack == nullptr) return 0.0;
    return static_cast<EEL_F> (rack->widthRight);
}

} // namespace vibesynth
