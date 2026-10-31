// M4 — LexiconFn5.cpp: bus_sat + bus_mono_low (registered in LexiconReg.cpp).
//
// bus_chorus / bus_reverb / bus_mono_low return the LEFT result; the RIGHT
// result is cached in the MasterRack and read by the matching _r() call
// (SPEC §4.3, same pattern as bus_mono_low).

#include "engine/LexiconArgs.h"

#include <cmath>

namespace vibesynth {

// bus_sat is valid on either rack type (it is stateless); the other bus_*
// need the master rack. It meters by call index through the VM's
// VoiceRack (the master VM has one too, and both master racks feed the
// master nodes), so L and R calls are separate nodes.
EEL_F NSEEL_CGEN_CALL fnBusSat (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* voice = voiceRackOf (ctx);
    auto* master = masterRackOf (ctx);
    if ((voice == nullptr && master == nullptr) || np < 2)
        return 0.0;
    const float out = satSample (
        sanitizeFloat (static_cast<float> (argD (p, np, 0, 0.0))),
        argD (p, np, 1, 0.0));
    if (voice != nullptr)
        voice->recordStateless (ModuleId::BusSat, out);
    else
        master->recordMeter (ModuleId::BusSat, 0, out);
    return static_cast<EEL_F> (out);
}

EEL_F NSEEL_CGEN_CALL fnBusMonoLow (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = masterRackOf (ctx);
    if (rack == nullptr || np < 2) return 0.0;
    const float l = sanitizeFloat (static_cast<float> (argD (p, np, 0, 0.0)));
    const float r = sanitizeFloat (static_cast<float> (argD (p, np, 1, 0.0)));
    const float loL = rack->monoXoverL.processLo (l);
    const float loR = rack->monoXoverR.processLo (r);
    const float mono = (loL + loR) * 0.5f;
    const float outL = sanitizeFloat (rack->monoXoverL.lastHi + mono);
    rack->monoRightHi = sanitizeFloat (rack->monoXoverR.lastHi + mono);
    rack->recordMeter (ModuleId::BusMonoLow, 0,
                       std::fmax (std::fabs (outL), std::fabs (rack->monoRightHi)));
    return static_cast<EEL_F> (outL);
}

EEL_F NSEEL_CGEN_CALL fnBusMonoLowR (void* ctx, INT_PTR, EEL_F**)
{
    auto* rack = masterRackOf (ctx);
    if (rack == nullptr) return 0.0;
    return static_cast<EEL_F> (rack->monoRightHi);
}

} // namespace vibesynth
