// M4 — LexiconReg.cpp: registerLexiconFunctions() + wrapper declarations.
// M8: the registrations are a table (engine/LexiconTable.h) that the lint
// and the api_reference.md smoketest read too.
// EEL2 headers must come first (they declare EEL_F/INT_PTR); our headers
// (which open namespace vibesynth) come after they close.

#include "eel2/ns-eel.h"
#include "eel2/ns-eel-addfuncs.h"

#include "engine/Lexicon.h"
#include "engine/LexiconTable.h"

namespace vibesynth {

// All fn* bodies are defined in the LexiconFnN.cpp files (plain vibesynth::
// definitions). Declare them here so registerLexiconFunctions() can take
// their addresses.
EEL_F NSEEL_CGEN_CALL fnOscSine (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnOscSaw (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnOscTri (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnOscSquare (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnOscSupersaw (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnNoise (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFltLp (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFltHp (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFltBp (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFltLadder (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnEnvAdsr (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnEnvFollow (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFxFold (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFxSat (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFxCrush (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFxComb (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFxOtt (void*, INT_PTR, EEL_F**);
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

namespace {

using LexiconFn = EEL_F (NSEEL_CGEN_CALL*) (void*, INT_PTR, EEL_F**);

struct Registration
{
    LexiconEntry entry;
    LexiconFn fn;
};

constexpr LexiconScope kBoth = LexiconScope::Both;
constexpr LexiconScope kMaster = LexiconScope::Master;

// SPEC §5.2. Argument counts are exact (the lint enforces them; EEL2's
// varparm registration only enforces the minimum). Slot counts mirror the
// pools in Lexicon.h. bus_sat is stateless and works on a voice rack too
// (LexiconFn5.cpp), so only the stateful bus_* are master-only.
const Registration kRegistrations[] = {
    { { "osc_sine", "slot, hz", 2, kNumOscSlots, kBoth }, &fnOscSine },
    { { "osc_saw", "slot, hz", 2, kNumOscSlots, kBoth }, &fnOscSaw },
    { { "osc_tri", "slot, hz", 2, kNumOscSlots, kBoth }, &fnOscTri },
    { { "osc_square", "slot, hz, pw", 3, kNumOscSlots, kBoth }, &fnOscSquare },
    { { "osc_supersaw", "slot, hz, detune, mix", 4, kNumSupersawSlots, kBoth }, &fnOscSupersaw },
    { { "noise", "", 0, 0, kBoth }, &fnNoise },
    { { "flt_lp", "slot, in, cutoff, res", 4, kNumSvfSlots, kBoth }, &fnFltLp },
    { { "flt_hp", "slot, in, cutoff, res", 4, kNumSvfSlots, kBoth }, &fnFltHp },
    { { "flt_bp", "slot, in, cutoff, res", 4, kNumSvfSlots, kBoth }, &fnFltBp },
    { { "flt_ladder", "slot, in, cutoff, res", 4, kNumLadderSlots, kBoth }, &fnFltLadder },
    { { "env_adsr", "slot, gate, a, d, s, r", 6, kNumAdsrSlots, kBoth }, &fnEnvAdsr },
    { { "env_follow", "slot, in", 2, kNumEnvFollowSlots, kBoth }, &fnEnvFollow },
    { { "fx_fold", "in, amount", 2, 0, kBoth }, &fnFxFold },
    { { "fx_sat", "in, drive", 2, 0, kBoth }, &fnFxSat },
    { { "fx_crush", "slot, in, bits, rate", 4, kNumCrushSlots, kBoth }, &fnFxCrush },
    { { "fx_comb", "slot, in, hz, feedback", 4, kNumCombSlots, kBoth }, &fnFxComb },
    { { "fx_ott", "slot, in, depth", 3, kNumOttSlots, kBoth }, &fnFxOtt },
    { { "phys_resonator", "slot, exciter, hz, structure, brightness, damping", 6, kNumResonatorSlots, kBoth }, &fnPhysResonator },
    { { "phys_string", "slot, exciter, hz, brightness, damping", 5, kNumStringSlots, kBoth }, &fnPhysString },
    { { "chaos_lorenz", "slot, rate, axis", 3, kNumLorenzSlots, kBoth }, &fnChaosLorenz },
    { { "chaos_duffing", "slot, hz, chaos", 3, kNumDuffingSlots, kBoth }, &fnChaosDuffing },
    { { "util_safe_fb", "slot, x", 2, kNumSafeFbSlots, kBoth }, &fnUtilSafeFb },
    { { "util_xover_lo", "slot, in", 2, kNumXoverSlots, kBoth }, &fnUtilXoverLo },
    { { "util_xover_hi", "slot", 1, kNumXoverSlots, kBoth }, &fnUtilXoverHi },
    { { "util_bytebeat", "t, mode", 2, 0, kBoth }, &fnUtilBytebeat },
    { { "bus_sat", "in, drive", 2, 0, kBoth }, &fnBusSat },
    { { "bus_mono_low", "l, r", 2, 0, kMaster }, &fnBusMonoLow },
    { { "bus_mono_low_r", "", 0, 0, kMaster }, &fnBusMonoLowR },
    { { "bus_chorus", "l, r, depth, mix", 4, 0, kMaster }, &fnBusChorus },
    { { "bus_chorus_r", "", 0, 0, kMaster }, &fnBusChorusR },
    { { "bus_reverb", "l, r, size, mix", 4, 0, kMaster }, &fnBusReverb },
    { { "bus_reverb_r", "", 0, 0, kMaster }, &fnBusReverbR },
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

void registerLexiconFunctions()
{
    for (const Registration& r : kRegistrations)
        NSEEL_addfunc_varparm (r.entry.name, r.entry.numArgs, NSEEL_PProc_THIS, r.fn);
}

} // namespace vibesynth
