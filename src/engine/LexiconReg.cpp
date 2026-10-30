// M4 — LexiconReg.cpp: registerLexiconFunctions() + wrapper declarations.
// EEL2 headers must come first (they declare EEL_F/INT_PTR); our headers
// (which open namespace vibesynth) come after they close.

#include "eel2/ns-eel.h"
#include "eel2/ns-eel-addfuncs.h"

#include "engine/Lexicon.h"

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

void registerLexiconFunctions()
{
    NSEEL_addfunc_varparm ("osc_sine", 2, NSEEL_PProc_THIS, &fnOscSine);
    NSEEL_addfunc_varparm ("osc_saw", 2, NSEEL_PProc_THIS, &fnOscSaw);
    NSEEL_addfunc_varparm ("osc_tri", 2, NSEEL_PProc_THIS, &fnOscTri);
    NSEEL_addfunc_varparm ("osc_square", 3, NSEEL_PProc_THIS, &fnOscSquare);
    NSEEL_addfunc_varparm ("osc_supersaw", 4, NSEEL_PProc_THIS, &fnOscSupersaw);
    NSEEL_addfunc_varparm ("noise", 0, NSEEL_PProc_THIS, &fnNoise);
    NSEEL_addfunc_varparm ("flt_lp", 4, NSEEL_PProc_THIS, &fnFltLp);
    NSEEL_addfunc_varparm ("flt_hp", 4, NSEEL_PProc_THIS, &fnFltHp);
    NSEEL_addfunc_varparm ("flt_bp", 4, NSEEL_PProc_THIS, &fnFltBp);
    NSEEL_addfunc_varparm ("flt_ladder", 4, NSEEL_PProc_THIS, &fnFltLadder);
    NSEEL_addfunc_varparm ("env_adsr", 6, NSEEL_PProc_THIS, &fnEnvAdsr);
    NSEEL_addfunc_varparm ("env_follow", 2, NSEEL_PProc_THIS, &fnEnvFollow);
    NSEEL_addfunc_varparm ("fx_fold", 2, NSEEL_PProc_THIS, &fnFxFold);
    NSEEL_addfunc_varparm ("fx_sat", 2, NSEEL_PProc_THIS, &fnFxSat);
    NSEEL_addfunc_varparm ("fx_crush", 4, NSEEL_PProc_THIS, &fnFxCrush);
    NSEEL_addfunc_varparm ("fx_comb", 4, NSEEL_PProc_THIS, &fnFxComb);
    NSEEL_addfunc_varparm ("fx_ott", 3, NSEEL_PProc_THIS, &fnFxOtt);
    NSEEL_addfunc_varparm ("phys_resonator", 6, NSEEL_PProc_THIS, &fnPhysResonator);
    NSEEL_addfunc_varparm ("phys_string", 5, NSEEL_PProc_THIS, &fnPhysString);
    NSEEL_addfunc_varparm ("chaos_lorenz", 3, NSEEL_PProc_THIS, &fnChaosLorenz);
    NSEEL_addfunc_varparm ("chaos_duffing", 3, NSEEL_PProc_THIS, &fnChaosDuffing);
    NSEEL_addfunc_varparm ("util_safe_fb", 2, NSEEL_PProc_THIS, &fnUtilSafeFb);
    NSEEL_addfunc_varparm ("util_xover_lo", 2, NSEEL_PProc_THIS, &fnUtilXoverLo);
    NSEEL_addfunc_varparm ("util_xover_hi", 1, NSEEL_PProc_THIS, &fnUtilXoverHi);
    NSEEL_addfunc_varparm ("util_bytebeat", 2, NSEEL_PProc_THIS, &fnUtilBytebeat);
    NSEEL_addfunc_varparm ("bus_sat", 2, NSEEL_PProc_THIS, &fnBusSat);
    NSEEL_addfunc_varparm ("bus_mono_low", 2, NSEEL_PProc_THIS, &fnBusMonoLow);
    NSEEL_addfunc_varparm ("bus_mono_low_r", 0, NSEEL_PProc_THIS, &fnBusMonoLowR);
    NSEEL_addfunc_varparm ("bus_chorus", 4, NSEEL_PProc_THIS, &fnBusChorus);
    NSEEL_addfunc_varparm ("bus_chorus_r", 0, NSEEL_PProc_THIS, &fnBusChorusR);
    NSEEL_addfunc_varparm ("bus_reverb", 4, NSEEL_PProc_THIS, &fnBusReverb);
    NSEEL_addfunc_varparm ("bus_reverb_r", 0, NSEEL_PProc_THIS, &fnBusReverbR);
}

} // namespace vibesynth
