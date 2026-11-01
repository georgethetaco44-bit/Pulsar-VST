// M4 — LexiconReg.cpp: registerLexiconFunctions() + wrapper declarations.
// M8: the registrations are a table (engine/LexiconTable.h) that the lint
// and the api_reference.md smoketest read too. M10: the table carries the
// inspector metadata, and every function is registered through a thunk
// that leads to the override/trace hook layer (LexiconHooks.h).
// EEL2 headers must come first (they declare EEL_F/INT_PTR); our headers
// (which open namespace vibesynth) come after they close.

#include "eel2/ns-eel.h"
#include "eel2/ns-eel-addfuncs.h"

#include <utility>

#include "engine/Lexicon.h"
#include "engine/LexiconHooks.h"
#include "engine/LexiconTable.h"
#include "engine/MasterRack.h"
#include "engine/NodeOverrides.h"

namespace vibesynth {

// All fn* bodies are defined in the LexiconFnN.cpp files (plain vibesynth::
// definitions). Declare them here so registerLexiconFunctions() can take
// their addresses.
EEL_F NSEEL_CGEN_CALL fnOscSine (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnOscSaw (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnOscTri (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnOscSquare (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnOscSupersaw (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnOscWt (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnOscFm (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnOscSync (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnNoise (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFltLp (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFltHp (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFltBp (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFltLadder (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFltFormant (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFltNotch (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFltPeak (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFltTilt (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnEnvAdsr (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnEnvFollow (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnModLfo (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFxFold (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFxSat (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFxShape (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFxCrush (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFxComb (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFxOtt (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFxPhaser (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFxFshift (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnPhysResonator (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnPhysString (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnChaosLorenz (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnChaosDuffing (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnUtilSafeFb (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnUtilXoverLo (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnUtilXoverHi (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnUtilBytebeat (void*, INT_PTR, EEL_F**);
// bus_* are plain vibesynth:: definitions (Fn5/Fn6 define them at
// namespace scope, not inside lexiconFn5/6).
EEL_F NSEEL_CGEN_CALL fnBusSat (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnBusMonoLow (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnBusMonoLowR (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnBusChorus (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnBusChorusR (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnBusReverb (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnBusReverbR (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnBusDelay (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnBusDelayR (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnBusWidth (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnBusWidthR (void*, INT_PTR, EEL_F**);

namespace {

using LexiconFn = EEL_F (NSEEL_CGEN_CALL*) (void*, INT_PTR, EEL_F**);

struct Registration
{
    LexiconEntry entry;
    LexiconFn fn;
};

constexpr LexiconScope kBoth = LexiconScope::Both;
constexpr LexiconScope kMaster = LexiconScope::Master;

// M10 argument metadata helpers (SPEC §9.6).
constexpr LexiconArg aSlot { "slot", "Slot", ArgKind::Slot, 0.0, 0.0 };
constexpr LexiconArg audio (const char* n) { return { n, n, ArgKind::Audio, 0.0, 0.0 }; }
constexpr LexiconArg gateArg (const char* n) { return { n, n, ArgKind::Gate, 0.0, 1.0 }; }
constexpr LexiconArg norm (const char* n, const char* label) { return { n, label, ArgKind::Norm, 0.0, 1.0 }; }
constexpr LexiconArg hz (const char* n, const char* label, double lo, double hi)
{
    return { n, label, ArgKind::Hz, lo, hi };
}
constexpr LexiconArg choice (const char* n, const char* label, double lo, double hi)
{
    return { n, label, ArgKind::Choice, lo, hi };
}
constexpr LexiconArg beats (const char* n, const char* label, double lo, double hi)
{
    return { n, label, ArgKind::Beats, lo, hi };
}
constexpr int mod (ModuleId m) { return static_cast<int> (m); }

constexpr BypassKind kNoBypass = BypassKind::None;
constexpr BypassKind kPass = BypassKind::PassArg;
constexpr BypassKind kZero = BypassKind::Zero;
constexpr BypassKind kForce = BypassKind::ForceArg;
constexpr TraceKind kAudio = TraceKind::Audio;
constexpr TraceKind kControl = TraceKind::Control;
constexpr TraceKind kByRate = TraceKind::ByRate;
constexpr TraceKind kNoTrace = TraceKind::None;

constexpr LexiconArg kPitch = hz ("hz", "Pitch", 20.0, 20000.0);

// SPEC §5.2. Argument counts are exact (the lint enforces them; EEL2's
// varparm registration only enforces the minimum). Slot counts mirror the
// pools in Lexicon.h. bus_sat is stateless and works on a voice rack too
// (LexiconFn5.cpp), so only the stateful bus_* are master-only.
//
// M10 columns after the scope: module, primary, bypass (kind, argument,
// value), trace (kind, rate argument), then one LexiconArg per argument.
// Bypass choices (SPEC §9.6): oscillators, chaos, bytebeat and the
// physical models (sound sources) output 0; util_safe_fb outputs 0
// (feedback off); filters, fx_* and the crossover pass their input;
// envelopes and bus_mono_low (two outputs, no input to pass) have none;
// bus_chorus, bus_reverb and bus_delay run with mix 0 (both channels dry),
// bus_width with amount 0.5 (unchanged, bit for bit).
constexpr Registration kRegistrations[] = {
    { { "osc_sine", "slot, hz", 2, kNumOscSlots, kBoth, mod (ModuleId::OscSine), true, kZero, 0, 0.0, kAudio, 0,
        { aSlot, kPitch } }, &fnOscSine },
    { { "osc_saw", "slot, hz", 2, kNumOscSlots, kBoth, mod (ModuleId::OscSaw), true, kZero, 0, 0.0, kAudio, 0,
        { aSlot, kPitch } }, &fnOscSaw },
    { { "osc_tri", "slot, hz", 2, kNumOscSlots, kBoth, mod (ModuleId::OscTri), true, kZero, 0, 0.0, kAudio, 0,
        { aSlot, kPitch } }, &fnOscTri },
    { { "osc_square", "slot, hz, pw", 3, kNumOscSlots, kBoth, mod (ModuleId::OscSquare), true, kZero, 0, 0.0, kAudio, 0,
        { aSlot, kPitch, norm ("pw", "Width") } }, &fnOscSquare },
    { { "osc_supersaw", "slot, hz, detune, mix", 4, kNumSupersawSlots, kBoth, mod (ModuleId::OscSupersaw), true,
        kZero, 0, 0.0, kAudio, 0,
        { aSlot, kPitch, norm ("detune", "Detune"), norm ("mix", "Mix") } }, &fnOscSupersaw },
    { { "osc_wt", "slot, hz, table, pos", 4, kNumWtOscSlots, kBoth, mod (ModuleId::OscWt), true, kZero, 0, 0.0,
        kAudio, 0, { aSlot, kPitch, choice ("table", "Table", 0.0, 7.0), norm ("pos", "Position") } }, &fnOscWt },
    { { "osc_fm", "slot, hz, ratio, index, feedback", 5, kNumFmOscSlots, kBoth, mod (ModuleId::OscFm), true,
        kZero, 0, 0.0, kAudio, 0,
        { aSlot, kPitch, norm ("ratio", "Ratio"), norm ("index", "Index"), norm ("feedback", "Feedback") } },
      &fnOscFm },
    { { "osc_sync", "slot, hz, ratio, shape", 4, kNumSyncOscSlots, kBoth, mod (ModuleId::OscSync), true, kZero, 0,
        0.0, kAudio, 0, { aSlot, kPitch, norm ("ratio", "Ratio"), norm ("shape", "Shape") } }, &fnOscSync },
    { { "noise", "", 0, 0, kBoth, -1, false, kNoBypass, 0, 0.0, kNoTrace, 0, {} }, &fnNoise },
    { { "flt_lp", "slot, in, cutoff, res", 4, kNumSvfSlots, kBoth, mod (ModuleId::FltLp), true, kPass, 1, 0.0, kAudio, 0,
        { aSlot, audio ("in"), norm ("cutoff", "Cutoff"), norm ("res", "Resonance") } }, &fnFltLp },
    { { "flt_hp", "slot, in, cutoff, res", 4, kNumSvfSlots, kBoth, mod (ModuleId::FltHp), true, kPass, 1, 0.0, kAudio, 0,
        { aSlot, audio ("in"), norm ("cutoff", "Cutoff"), norm ("res", "Resonance") } }, &fnFltHp },
    { { "flt_bp", "slot, in, cutoff, res", 4, kNumSvfSlots, kBoth, mod (ModuleId::FltBp), true, kPass, 1, 0.0, kAudio, 0,
        { aSlot, audio ("in"), norm ("cutoff", "Cutoff"), norm ("res", "Resonance") } }, &fnFltBp },
    { { "flt_ladder", "slot, in, cutoff, res", 4, kNumLadderSlots, kBoth, mod (ModuleId::FltLadder), true, kPass, 1, 0.0,
        kAudio, 0, { aSlot, audio ("in"), norm ("cutoff", "Cutoff"), norm ("res", "Resonance") } }, &fnFltLadder },
    { { "flt_formant", "slot, in, vowel, res", 4, kNumFormantSlots, kBoth, mod (ModuleId::FltFormant), true, kPass,
        1, 0.0, kAudio, 0, { aSlot, audio ("in"), norm ("vowel", "Vowel"), norm ("res", "Resonance") } },
      &fnFltFormant },
    { { "flt_notch", "slot, in, cutoff, res", 4, kNumNotchSlots, kBoth, mod (ModuleId::FltNotch), true, kPass, 1,
        0.0, kAudio, 0, { aSlot, audio ("in"), norm ("cutoff", "Cutoff"), norm ("res", "Resonance") } }, &fnFltNotch },
    { { "flt_peak", "slot, in, cutoff, gain, res", 5, kNumPeakSlots, kBoth, mod (ModuleId::FltPeak), true, kPass, 1,
        0.0, kAudio, 0,
        { aSlot, audio ("in"), norm ("cutoff", "Cutoff"), norm ("gain", "Gain"), norm ("res", "Resonance") } },
      &fnFltPeak },
    { { "flt_tilt", "slot, in, tilt", 3, kNumTiltSlots, kBoth, mod (ModuleId::FltTilt), true, kPass, 1, 0.0, kAudio,
        0, { aSlot, audio ("in"), norm ("tilt", "Tilt") } }, &fnFltTilt },
    { { "env_adsr", "slot, gate, a, d, s, r", 6, kNumAdsrSlots, kBoth, mod (ModuleId::EnvAdsr), true, kNoBypass, 0, 0.0,
        kControl, 0,
        { aSlot, gateArg ("gate"), norm ("a", "Attack"), norm ("d", "Decay"), norm ("s", "Sustain"),
          norm ("r", "Release") } }, &fnEnvAdsr },
    { { "env_follow", "slot, in", 2, kNumEnvFollowSlots, kBoth, mod (ModuleId::EnvFollow), true, kNoBypass, 0, 0.0,
        kControl, 0, { aSlot, audio ("in") } }, &fnEnvFollow },
    { { "mod_lfo", "slot, beats, shape", 3, kNumModLfoSlots, kBoth, mod (ModuleId::ModLfo), true, kNoBypass, 0, 0.0,
        kControl, 0, { aSlot, beats ("beats", "Beats", 0.0625, 16.0), norm ("shape", "Shape") } }, &fnModLfo },
    { { "fx_fold", "in, amount", 2, 0, kBoth, mod (ModuleId::FxFold), true, kPass, 0, 0.0, kAudio, 0,
        { audio ("in"), norm ("amount", "Amount") } }, &fnFxFold },
    { { "fx_sat", "in, drive", 2, 0, kBoth, mod (ModuleId::FxSat), true, kPass, 0, 0.0, kAudio, 0,
        { audio ("in"), norm ("drive", "Drive") } }, &fnFxSat },
    { { "fx_shape", "in, drive, type", 3, 0, kBoth, mod (ModuleId::FxShape), true, kPass, 0, 0.0, kAudio, 0,
        { audio ("in"), norm ("drive", "Drive"), norm ("type", "Type") } }, &fnFxShape },
    { { "fx_crush", "slot, in, bits, rate", 4, kNumCrushSlots, kBoth, mod (ModuleId::FxCrush), true, kPass, 1, 0.0,
        kAudio, 0, { aSlot, audio ("in"), norm ("bits", "Bits"), norm ("rate", "Rate") } }, &fnFxCrush },
    { { "fx_comb", "slot, in, hz, feedback", 4, kNumCombSlots, kBoth, mod (ModuleId::FxComb), true, kPass, 1, 0.0,
        kAudio, 0, { aSlot, audio ("in"), hz ("hz", "Pitch", 20.0, 5000.0), norm ("feedback", "Feedback") } },
      &fnFxComb },
    { { "fx_ott", "slot, in, depth", 3, kNumOttSlots, kBoth, mod (ModuleId::FxOtt), true, kPass, 1, 0.0, kAudio, 0,
        { aSlot, audio ("in"), norm ("depth", "Depth") } }, &fnFxOtt },
    { { "fx_phaser", "slot, in, pos, depth, feedback", 5, kNumPhaserSlots, kBoth, mod (ModuleId::FxPhaser), true,
        kPass, 1, 0.0, kAudio, 0,
        { aSlot, audio ("in"), norm ("pos", "Position"), norm ("depth", "Depth"), norm ("feedback", "Feedback") } },
      &fnFxPhaser },
    { { "fx_fshift", "slot, in, shift", 3, kNumFshiftSlots, kBoth, mod (ModuleId::FxFshift), true, kPass, 1, 0.0,
        kAudio, 0, { aSlot, audio ("in"), norm ("shift", "Shift") } }, &fnFxFshift },
    { { "phys_resonator", "slot, exciter, hz, structure, brightness, damping", 6, kNumResonatorSlots, kBoth,
        mod (ModuleId::PhysResonator), true, kZero, 0, 0.0, kAudio, 0,
        { aSlot, audio ("exciter"), hz ("hz", "Pitch", 20.0, 8000.0), norm ("structure", "Structure"),
          norm ("brightness", "Brightness"), norm ("damping", "Damping") } }, &fnPhysResonator },
    { { "phys_string", "slot, exciter, hz, brightness, damping", 5, kNumStringSlots, kBoth,
        mod (ModuleId::PhysString), true, kZero, 0, 0.0, kAudio, 0,
        { aSlot, audio ("exciter"), hz ("hz", "Pitch", 40.0, 4000.0), norm ("brightness", "Brightness"),
          norm ("damping", "Damping") } }, &fnPhysString },
    { { "chaos_lorenz", "slot, rate, axis", 3, kNumLorenzSlots, kBoth, mod (ModuleId::ChaosLorenz), true, kZero, 0,
        0.0, kByRate, 1, { aSlot, norm ("rate", "Rate"), choice ("axis", "Axis", 0.0, 2.0) } }, &fnChaosLorenz },
    { { "chaos_duffing", "slot, hz, chaos", 3, kNumDuffingSlots, kBoth, mod (ModuleId::ChaosDuffing), true, kZero, 0,
        0.0, kByRate, 1, { aSlot, hz ("hz", "Rate", 0.1, 2000.0), norm ("chaos", "Chaos") } }, &fnChaosDuffing },
    { { "util_safe_fb", "slot, x", 2, kNumSafeFbSlots, kBoth, mod (ModuleId::UtilSafeFb), true, kZero, 0, 0.0,
        kAudio, 0, { aSlot, audio ("x") } }, &fnUtilSafeFb },
    { { "util_xover_lo", "slot, in", 2, kNumXoverSlots, kBoth, mod (ModuleId::UtilXover), true, kPass, 1, 0.0,
        kAudio, 0, { aSlot, audio ("in") } }, &fnUtilXoverLo },
    { { "util_xover_hi", "slot", 1, kNumXoverSlots, kBoth, mod (ModuleId::UtilXover), false, kZero, 0, 0.0,
        kNoTrace, 0, { aSlot } }, &fnUtilXoverHi },
    { { "util_bytebeat", "t, mode", 2, 0, kBoth, mod (ModuleId::UtilBytebeat), true, kZero, 0, 0.0, kAudio, 0,
        { audio ("t"), choice ("mode", "Mode", 0.0, 3.0) } }, &fnUtilBytebeat },
    { { "bus_sat", "in, drive", 2, 0, kBoth, mod (ModuleId::BusSat), true, kPass, 0, 0.0, kAudio, 0,
        { audio ("in"), norm ("drive", "Drive") } }, &fnBusSat },
    { { "bus_mono_low", "l, r", 2, 0, kMaster, mod (ModuleId::BusMonoLow), true, kNoBypass, 0, 0.0, kAudio, 0,
        { audio ("l"), audio ("r") } }, &fnBusMonoLow },
    { { "bus_mono_low_r", "", 0, 0, kMaster, mod (ModuleId::BusMonoLow), false, kNoBypass, 0, 0.0, kNoTrace, 0, {} },
      &fnBusMonoLowR },
    { { "bus_chorus", "l, r, depth, mix", 4, 0, kMaster, mod (ModuleId::BusChorus), true, kForce, 3, 0.0, kAudio, 0,
        { audio ("l"), audio ("r"), norm ("depth", "Depth"), norm ("mix", "Mix") } }, &fnBusChorus },
    { { "bus_chorus_r", "", 0, 0, kMaster, mod (ModuleId::BusChorus), false, kNoBypass, 0, 0.0, kNoTrace, 0, {} },
      &fnBusChorusR },
    { { "bus_reverb", "l, r, size, mix", 4, 0, kMaster, mod (ModuleId::BusReverb), true, kForce, 3, 0.0, kAudio, 0,
        { audio ("l"), audio ("r"), norm ("size", "Size"), norm ("mix", "Mix") } }, &fnBusReverb },
    { { "bus_reverb_r", "", 0, 0, kMaster, mod (ModuleId::BusReverb), false, kNoBypass, 0, 0.0, kNoTrace, 0, {} },
      &fnBusReverbR },
    { { "bus_delay", "l, r, beats, feedback, mix", 5, 0, kMaster, mod (ModuleId::BusDelay), true, kForce, 4, 0.0,
        kAudio, 0,
        { audio ("l"), audio ("r"), beats ("beats", "Time", DelayBusState::kMinBeats, DelayBusState::kMaxBeats),
          norm ("feedback", "Feedback"), norm ("mix", "Mix") } }, &fnBusDelay },
    { { "bus_delay_r", "", 0, 0, kMaster, mod (ModuleId::BusDelay), false, kNoBypass, 0, 0.0, kNoTrace, 0, {} },
      &fnBusDelayR },
    { { "bus_width", "l, r, amount", 3, 0, kMaster, mod (ModuleId::BusWidth), true, kForce, 2, 0.5, kAudio, 0,
        { audio ("l"), audio ("r"), norm ("amount", "Width") } }, &fnBusWidth },
    { { "bus_width_r", "", 0, 0, kMaster, mod (ModuleId::BusWidth), false, kNoBypass, 0, 0.0, kNoTrace, 0, {} },
      &fnBusWidthR },
};

constexpr int kNumRegistrations = static_cast<int> (sizeof (kRegistrations) / sizeof (kRegistrations[0]));

bool sameNameIgnoringCase (const char* a, const char* b) noexcept
{
    for (; *a != 0 && *b != 0; ++a, ++b)
    {
        const char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char> (*a - 'A' + 'a') : *a;
        const char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char> (*b - 'A' + 'a') : *b;
        if (ca != cb)
            return false;
    }
    return *a == *b;
}

} // namespace

int numLexiconEntries() noexcept
{
    return kNumRegistrations;
}

const LexiconEntry& lexiconEntry (int index) noexcept
{
    return kRegistrations[index >= 0 && index < kNumRegistrations ? index : 0].entry;
}

const LexiconEntry* findLexiconEntry (const char* name) noexcept
{
    if (name == nullptr)
        return nullptr;
    for (const Registration& r : kRegistrations)
        if (sameNameIgnoringCase (r.entry.name, name))
            return &r.entry;
    return nullptr;
}

const LexiconEntry* primaryLexiconEntry (int module) noexcept
{
    for (const Registration& r : kRegistrations)
        if (r.entry.primary && r.entry.module == module)
            return &r.entry;
    return nullptr;
}

int lexiconArgIndex (const LexiconEntry& e, const char* name) noexcept
{
    if (name == nullptr)
        return -1;
    for (int i = 0; i < e.numArgs && i < kMaxLexiconArgs; ++i)
        if (sameNameIgnoringCase (e.args[i].name, name))
            return i;
    return -1;
}

namespace {

// M10 (SPEC §9.6): every function is registered through a thunk. With no
// override set and no trace wanted on the calling rack (`hooked` false,
// VmRackContext::hooked, set per chunk by PatchPlayer) it tail-calls the
// wrapper unchanged: one load and one predictable branch. Otherwise the shared hook layer
// (LexiconHooks.cpp) applies the node's overrides and bypass, calls the
// same wrapper and records the trace.
//
// M13: a hooked VM still skips the hook layer for calls whose module has
// nothing set and no trace wanted (VmRackContext::hookModules, one load).
// callHooked() would find no node and no sink and run the wrapper on the
// same arguments, so this is bit-identical; it saves the argument copy and
// lookups (~7 ns a call), which matters once one modulated knob hooks
// every call of the voice.
// M13: the hooked call for a slot module, specialized per function: the
// common case (no trace, no bypass) inline with the argument count, kinds
// and slot range as constants, and only the set arguments visited. The
// same checks and maths as callHooked(), which takes every other case.
#if defined(_MSC_VER)
 #define VIBESYNTH_NOINLINE __declspec(noinline)
#else
 #define VIBESYNTH_NOINLINE __attribute__ ((noinline))
#endif

// Out of line, so the unhooked thunk stays one load, one branch and a tail
// call (inlined, its frame would be set up on every call).
template <int I>
VIBESYNTH_NOINLINE EEL_F hookedCall (void* ctx, INT_PTR np, EEL_F** p) noexcept
{
    constexpr const LexiconEntry& e = kRegistrations[I].entry;
    constexpr LexiconFn fn = kRegistrations[I].fn;
    if constexpr (e.primary && e.numSlots > 0 && e.numArgs >= 1 && e.numArgs <= kMaxLexiconArgs
                  && e.scope != LexiconScope::Master)
    {
        const VoiceRack* vr = static_cast<const VmRackContext*> (ctx)->voice;
        bool plain = np >= e.numArgs && vr != nullptr && vr->capture == nullptr && vr->overrides != nullptr;
        for (int i = 0; plain && i < e.numArgs; ++i)
            plain = p[i] != nullptr;
        if (plain)
        {
            const int slot = clampSlot (static_cast<double> (*p[0]), e.numSlots);
            const NodeOverride* node = vr->overrides->activeNode (vr->rackKind, e.module, slot);
            if (node == nullptr)
                return fn (ctx, np, p);
            if (e.bypass == BypassKind::None || node->bypass.load (std::memory_order_relaxed) == 0)
            {
                EEL_F vals[kMaxLexiconArgs];
                EEL_F* ptrs[kMaxLexiconArgs];
                const uint32_t bits = node->argBits.load (std::memory_order_acquire);
                for (int i = 0; i < e.numArgs; ++i)
                {
                    ptrs[i] = p[i];
                    if (isEditableArg (e.args[i].kind) && ((bits >> i) & 1u) != 0u)
                    {
                        vals[i] = static_cast<EEL_F> (effectiveArg (e.args[i], node->args[i], vr->macroVars,
                                                                    vr->modValues, static_cast<double> (*p[i])));
                        ptrs[i] = &vals[i];
                    }
                }
                return fn (ctx, static_cast<INT_PTR> (e.numArgs), ptrs);
            }
        }
    }
    return callHooked (e, fn, ctx, np, p);
}

template <int I>
EEL_F NSEEL_CGEN_CALL thunk (void* ctx, INT_PTR np, EEL_F** p)
{
    // Every VM sets its context (PatchEngine.cpp), so ctx is never null.
    constexpr int module = kRegistrations[I].entry.module;
    const auto* rc = static_cast<const VmRackContext*> (ctx);
    if constexpr (module >= 0 && module < 64)
        if (rc->hooked && ((rc->hookModules >> module) & 1u) != 0u)
        {
            // The trace voice (an editor is open) takes M10's full path directly.
            if (rc->voice != nullptr && rc->voice->capture != nullptr)
                return callHooked (kRegistrations[I].entry, kRegistrations[I].fn, ctx, np, p);
            return hookedCall<I> (ctx, np, p);
        }
    return kRegistrations[I].fn (ctx, np, p);
}

// A plain array (std::array<LexiconFn> drops the calling-convention
// attribute on some targets).
template <int... I>
struct ThunkTable
{
    static constexpr LexiconFn fns[sizeof... (I)] = { &thunk<I>... };
};

template <int... I>
constexpr ThunkTable<I...> thunkTable (std::integer_sequence<int, I...>)
{
    return {};
}

using Thunks = decltype (thunkTable (std::make_integer_sequence<int, kNumRegistrations>()));

} // namespace

void registerLexiconFunctions()
{
    initWavetables();
    for (int i = 0; i < kNumRegistrations; ++i)
        NSEEL_addfunc_varparm (kRegistrations[i].entry.name, kRegistrations[i].entry.numArgs, NSEEL_PProc_THIS,
                               Thunks::fns[i]);
}

} // namespace vibesynth
