// M4 — LexiconFn4.cpp: chaos_* + util_* voice functions.

#include "engine/LexiconArgs.h"

#include <cmath>

namespace vibesynth {

EEL_F NSEEL_CGEN_CALL fnChaosLorenz (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 3) return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumLorenzSlots);
    const float rate = static_cast<float> (clamp01 (argD (p, np, 1, 0.0)));
    const double axisIn = argD (p, np, 2, 0.0);
    const int axis = (! isFiniteBitsD (axisIn) || axisIn < 0.0)
                     ? 0 : (axisIn > 2.0 ? 2 : static_cast<int> (axisIn));
    auto& st = rack->lorenz[slot];
    auto& cc = rack->lorenzCache[slot];
    if (rate != cc.rate)
    {
        // rate 0–1 → orbit frequency 0.05 Hz (slow LFO) … 1 kHz (audio),
        // exponential. Lorenz time per sample = orbitHz / (1.62 · sr),
        // capped at kMaxSubsteps × kMaxSubstepDt (0.02 → ~1.5 kHz orbits
        // at 48 kHz), split into ≤ 4 RK2 substeps of ≤ 0.005.
        const double orbitHz = 0.05 * std::pow (20000.0, static_cast<double> (rate));
        double dtSample = orbitHz / (LorenzState::kCyclesPerTimeUnit * rack->sampleRate);
        const double maxDt = LorenzState::kMaxSubsteps * LorenzState::kMaxSubstepDt;
        if (dtSample > maxDt) dtSample = maxDt;
        int steps = static_cast<int> (std::ceil (dtSample / LorenzState::kMaxSubstepDt));
        if (steps < 1) steps = 1;
        if (steps > LorenzState::kMaxSubsteps) steps = LorenzState::kMaxSubsteps;
        cc.rate = rate;
        cc.steps = steps;
        cc.dt = dtSample / static_cast<double> (steps);
    }
    for (int i = 0; i < cc.steps; ++i)
        st.step (cc.dt);
    double v = 0.0;
    if (axis == 0) v = st.x / 24.0;
    else if (axis == 1) v = st.y / 24.0;
    else v = st.z / 48.0;
    if (v > 1.0) v = 1.0; else if (v < -1.0) v = -1.0;
    const float out = sanitizeFloat (static_cast<float> (v));
    rack->recordMeter (ModuleId::ChaosLorenz, slot, out);
    return static_cast<EEL_F> (out);
}

EEL_F NSEEL_CGEN_CALL fnChaosDuffing (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 3) return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumDuffingSlots);
    // hz is capped where one sample spans 4 substeps of 0.1 time units
    // (0.076 × sr ≈ 3.7 kHz at 48 kHz) so the integrator stays stable.
    constexpr double kTwoPi = 6.28318530717958647692;
    const double sr = rack->sampleRate;
    const double maxHz = DuffingState::kMaxSubsteps * DuffingState::kMaxSubstepDt
                         * DuffingState::kForcingOmega * sr / kTwoPi;
    double hz = clampHz (argD (p, np, 1, 2.0), sr);
    if (hz > maxHz) hz = maxHz;
    const double chaos = clamp01 (argD (p, np, 2, 0.5));
    auto& st = rack->duffing[slot];
    auto& cc = rack->duffingCache[slot];
    const float hzf = static_cast<float> (hz);
    if (hzf != cc.hz)
    {
        const double theta = kTwoPi * static_cast<double> (hzf) / sr;
        const double tau = theta / DuffingState::kForcingOmega; // time units/sample
        int steps = static_cast<int> (std::ceil (tau / DuffingState::kMaxSubstepDt));
        if (steps < 1) steps = 1;
        if (steps > DuffingState::kMaxSubsteps) steps = DuffingState::kMaxSubsteps;
        double dcHz = 0.1 * static_cast<double> (hzf);
        if (dcHz < 0.05) dcHz = 0.05;
        cc.hz = hzf;
        cc.rotCos = std::cos (theta);
        cc.rotSin = std::sin (theta);
        cc.steps = steps;
        cc.h = tau / static_cast<double> (steps);
        cc.dcR = 1.0 - kTwoPi * dcHz / sr;
    }
    const double gamma = 0.20 + 0.30 * chaos;
    double v = 0.6 * st.step (gamma, cc.rotCos, cc.rotSin, cc.h, cc.steps, cc.dcR);
    if (v > 1.0) v = 1.0; else if (v < -1.0) v = -1.0;
    const float out = sanitizeFloat (static_cast<float> (v));
    rack->recordMeter (ModuleId::ChaosDuffing, slot, out);
    return static_cast<EEL_F> (out);
}

EEL_F NSEEL_CGEN_CALL fnUtilSafeFb (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 2) return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumSafeFbSlots);
    const float out = rack->safeFb[slot].process (
        sanitizeFloat (static_cast<float> (argD (p, np, 1, 0.0))),
        rack->sampleRate);
    rack->recordMeter (ModuleId::UtilSafeFb, slot, out);
    return static_cast<EEL_F> (out);
}

EEL_F NSEEL_CGEN_CALL fnUtilXoverLo (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 2) return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumXoverSlots);
    const float out = rack->xover[slot].processLo (sanitizeFloat (
        static_cast<float> (argD (p, np, 1, 0.0))));
    rack->recordMeter (ModuleId::UtilXover, slot, out);
    return static_cast<EEL_F> (out);
}

EEL_F NSEEL_CGEN_CALL fnUtilXoverHi (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 1) return 0.0;
    const int slot = clampSlot (argD (p, np, 0, 0.0), kNumXoverSlots);
    const float out = rack->xover[slot].lastHi;
    rack->recordMeter (ModuleId::UtilXover, slot, out);
    return static_cast<EEL_F> (out);
}

EEL_F NSEEL_CGEN_CALL fnUtilBytebeat (void* ctx, INT_PTR np, EEL_F** p)
{
    auto* rack = voiceRackOf (ctx);
    if (rack == nullptr || np < 2) return 0.0;
    const float out = bytebeatSample (argD (p, np, 0, 0.0), argD (p, np, 1, 0.0));
    rack->recordStateless (ModuleId::UtilBytebeat, out);
    return static_cast<EEL_F> (out);
}

} // namespace vibesynth
