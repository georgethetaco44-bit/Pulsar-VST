// M13 smoketests (SPEC §9.8). See SmokeM13.h.

#include "SmokeM13.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "CliCheck.h"
#include "ai/Generator.h"
#include "ai/Mutator.h"
#include "ai/ResponseParser.h"
#include "ai/UndoStack.h"
#include "engine/Modulators.h"
#include "engine/NodeOverrides.h"
#include "patch/Patch.h"
#include "patch/PatchBake.h"
#include "patch/PatchPlayer.h"
#include "patch/PatchTweaks.h"
#include "patch/PatchValidator.h"

namespace vibesynth_cli {

namespace {

using namespace vibesynth;

constexpr double kRate = 48000.0;

struct Ev
{
    int samplePosition;
    const uint8_t* data;
    int numBytes;
};

Patch builtin (const char* stem)
{
    for (int i = 0; i < numBuiltinPresets(); ++i)
        if (juce::String (builtinPresetFile (i)) == juce::String (stem) + ".pulsar")
            return parsePatch (builtinPresetText (i)).patch;
    return {};
}

std::string replaced (const std::string& s, const std::string& from, const std::string& to)
{
    return juce::String (s).replace (from, to).toStdString();
}

bool sameBits (double a, double b)
{
    return std::memcmp (&a, &b, sizeof (double)) == 0;
}

bool sameFloatBits (const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0);
}

bool isFiniteFloat (float v)
{
    uint32_t b = 0;
    std::memcpy (&b, &v, sizeof (v));
    return ((b >> 23) & 0xFFu) != 0xFFu;
}

bool isFiniteDouble (double v)
{
    uint64_t b = 0;
    std::memcpy (&b, &v, sizeof (v));
    return ((b >> 52) & 0x7FFull) != 0x7FFull;
}

std::vector<float> renderPhrase (const Patch& patch, const OverrideTable* table, double seconds = 1.6)
{
    std::string error;
    auto program = PatchProgram::build (patch, kRate, error);
    std::vector<float> out;
    if (program == nullptr)
        return out;
    float targets[PatchPlayer::kNumMacros];
    for (int m = 0; m < PatchPlayer::kNumMacros; ++m)
        targets[m] = static_cast<float> (patch.macroDefault (m));
    PatchPlayer player;
    player.setProgram (program.get());
    player.prepare (kRate, 256, targets);
    player.setHostTransport (140.0, false, false, 0.0);
    player.setVoiceControls (PatchPlayer::VoiceMode::Patch, 0.2);
    player.setNodeHooks (table, nullptr);

    static const uint8_t on40[3] = { 0x90, 40, 100 }, off40[3] = { 0x80, 40, 0 };
    static const uint8_t on45[3] = { 0x90, 45, 90 }, off45[3] = { 0x80, 45, 0 };
    static const uint8_t on52[3] = { 0x90, 52, 90 }, off52[3] = { 0x80, 52, 0 };
    auto at = [] (double s) { return static_cast<int> (std::lround (s * kRate)); };
    const std::vector<Ev> events = { { at (0.0), on40, 3 }, { at (0.5), off40, 3 },
                                     { at (0.6), on45, 3 }, { at (0.6), on52, 3 },
                                     { at (1.3), off45, 3 }, { at (1.3), off52, 3 } };
    auto it = events.cbegin();
    const int total = at (seconds);
    for (int start = 0; start < total; start += 256)
    {
        const double t = start / kRate;
        for (int m = 0; m < PatchPlayer::kNumMacros; ++m)
            targets[m] = static_cast<float> (0.5 + 0.45 * std::sin (6.2831853 * t * (0.3 + 0.17 * m) + m));
        const int n = std::min (256, total - start);
        player.renderChunk (n, targets, it, events.cend(), start);
        for (int i = 0; i < n; ++i)
        {
            out.push_back (player.outL()[i]);
            out.push_back (player.outR()[i]);
        }
    }
    return out;
}

//==============================================================================
// 1. LFO shapes, rates, host sync, retrigger vs free, S&H and smooth random.
bool smokeLfoShapesAndRates()
{
    bool ok = true;

    // Reference values at phases 0.0, 0.25, 0.5, 0.75 for the 5 deterministic shapes.
    // The sine peaks use 1e-6: fastSin is a degree-11 polynomial (M12), ~6e-8 off at pi/2.
    const double s0 = LfoVoiceState::evalShape (LfoShape::Sine, 0.0, 0, 1u);
    const double s25 = LfoVoiceState::evalShape (LfoShape::Sine, 0.25, 0, 1u);
    const double s50 = LfoVoiceState::evalShape (LfoShape::Sine, 0.5, 0, 1u);
    const double s75 = LfoVoiceState::evalShape (LfoShape::Sine, 0.75, 0, 1u);
    const double t0 = LfoVoiceState::evalShape (LfoShape::Triangle, 0.0, 0, 1u);
    const double t25 = LfoVoiceState::evalShape (LfoShape::Triangle, 0.25, 0, 1u);
    const double t50 = LfoVoiceState::evalShape (LfoShape::Triangle, 0.5, 0, 1u);
    const double t75 = LfoVoiceState::evalShape (LfoShape::Triangle, 0.75, 0, 1u);
    const double u0 = LfoVoiceState::evalShape (LfoShape::SawUp, 0.0, 0, 1u);
    const double u50 = LfoVoiceState::evalShape (LfoShape::SawUp, 0.5, 0, 1u);
    const double d0 = LfoVoiceState::evalShape (LfoShape::SawDown, 0.0, 0, 1u);
    const double d50 = LfoVoiceState::evalShape (LfoShape::SawDown, 0.5, 0, 1u);
    const double q10 = LfoVoiceState::evalShape (LfoShape::Square, 0.1, 0, 1u);
    const double q60 = LfoVoiceState::evalShape (LfoShape::Square, 0.6, 0, 1u);

    const bool detOk = std::fabs (s0) < 1e-12 && std::fabs (s25 - 1.0) < 1e-6 && std::fabs (s50) < 1e-9
                    && std::fabs (s75 + 1.0) < 1e-6 && std::fabs (t0) < 1e-12 && std::fabs (t25 - 1.0) < 1e-12
                    && std::fabs (t50) < 1e-12 && std::fabs (t75 + 1.0) < 1e-12 && std::fabs (u0 + 1.0) < 1e-12
                    && std::fabs (u50) < 1e-12 && std::fabs (d0 - 1.0) < 1e-12 && std::fabs (d50) < 1e-12
                    && q10 == 1.0 && q60 == -1.0;

    // Sample & Hold stays constant within a cycle and steps on the next cycle.
    const double shA = LfoVoiceState::evalShape (LfoShape::SampleAndHold, 0.1, 7, 42u);
    const double shB = LfoVoiceState::evalShape (LfoShape::SampleAndHold, 0.9, 7, 42u);
    const double shC = LfoVoiceState::evalShape (LfoShape::SampleAndHold, 0.1, 8, 42u);
    // Smooth Random is C1-continuous across cycle boundaries.
    const double srEnd = LfoVoiceState::evalShape (LfoShape::SmoothRandom, 0.9999, 7, 42u);
    const double srNext = LfoVoiceState::evalShape (LfoShape::SmoothRandom, 0.0001, 8, 42u);
    ok = check (detOk && shA == shB && shA != shC && std::fabs (srEnd - srNext) < 1e-4,
                "M13 LFO shapes: sine/tri/saw_up/saw_down/square match reference points, S&H holds per cycle "
                "(%.3f -> %.3f), smooth random is continuous across cycles (|diff| %.2e)",
                shA, shC, std::fabs (srEnd - srNext))
       && ok;

    // Sync divisions: 1/32 to 4 bars with dotted and triplet, name round-trip.
    constexpr int numDivs = static_cast<int> (LfoSyncDiv::Count);
    int divRoundTrip = 0;
    for (int i = 0; i < numDivs; ++i)
    {
        LfoSyncDiv parsed = LfoSyncDiv::Div1_4;
        const auto div = static_cast<LfoSyncDiv> (i);
        if (parseLfoSyncDivId (lfoSyncDivId (div), parsed) && parsed == div && lfoSyncDivBeats (div) > 0.0)
            ++divRoundTrip;
    }
    const bool divMath = std::fabs (lfoSyncDivBeats (LfoSyncDiv::Div1_32) - 0.125) < 1e-12
                      && std::fabs (lfoSyncDivBeats (LfoSyncDiv::Div1_4) - 1.0) < 1e-12
                      && std::fabs (lfoSyncDivBeats (LfoSyncDiv::Div1_4D) - 1.5) < 1e-12
                      && std::fabs (lfoSyncDivBeats (LfoSyncDiv::Div1_4T) - 2.0 / 3.0) < 1e-12
                      && std::fabs (lfoSyncDivBeats (LfoSyncDiv::Div4_1) - 16.0) < 1e-12;
    ok = check (divRoundTrip == numDivs && divMath,
                "M13 LFO sync divisions: %d/%d divisions (1/32 to 4 bars, dotted and triplet) round-trip and match "
                "exact beat lengths",
                divRoundTrip, numDivs)
       && ok;

    // Free Hz rate vs scripted host-beat sync, and S&H / smooth random range over 2 seconds.
    LfoVoiceState freeHz;
    freeHz.prepare (kRate, 0, 1);
    freeHz.noteOn (60);
    ModLfoSpec specHz;
    specHz.shape = LfoShape::Sine;
    specHz.sync = false;
    specHz.rateHz = 5.0;
    specHz.retrigger = true;
    int crossings = 0;
    double prev = 0.0, phaseOut = 0.0;
    for (int i = 0; i < 48000; ++i)
    {
        const double v = freeHz.step (specHz, 120.0, i * (2.0 / 48000.0), phaseOut);
        if (i > 0 && prev <= 0.0 && v > 0.0)
            ++crossings;
        prev = v;
    }

    // Scripted host position: jumping PPQ to beat 3.25 puts a 1/4-note synced free LFO at phase 0.25 (+1.0 sine).
    LfoVoiceState hostSynced;
    hostSynced.prepare (kRate, 0, 2);
    ModLfoSpec specSync;
    specSync.shape = LfoShape::Sine;
    specSync.sync = true;
    specSync.syncDiv = LfoSyncDiv::Div1_4;
    specSync.retrigger = false;
    double syncPhase = 0.0;
    for (int i = 0; i < 200; ++i)
        hostSynced.step (specSync, 120.0, 3.25, syncPhase);
    const double atQuarter = hostSynced.step (specSync, 120.0, 3.25, syncPhase);

    // S&H and SmoothRandom stay in [-1, 1] and move across their range.
    LfoVoiceState shState, srState;
    shState.prepare (kRate, 0, 3);
    srState.prepare (kRate, 1, 4);
    ModLfoSpec shSpec = specHz, srSpec = specHz;
    shSpec.shape = LfoShape::SampleAndHold;
    shSpec.rateHz = 25.0;
    srSpec.shape = LfoShape::SmoothRandom;
    srSpec.rateHz = 25.0;
    double shMin = 1.0, shMax = -1.0, srMin = 1.0, srMax = -1.0;
    bool inRange = true;
    for (int i = 0; i < 96000; ++i)
    {
        double p1 = 0.0, p2 = 0.0;
        const double a = shState.step (shSpec, 120.0, 0.0, p1);
        const double b = srState.step (srSpec, 120.0, 0.0, p2);
        if (! isFiniteDouble (a) || ! isFiniteDouble (b) || a < -1.0 || a > 1.0 || b < -1.0 || b > 1.0)
            inRange = false;
        shMin = std::min (shMin, a);
        shMax = std::max (shMax, a);
        srMin = std::min (srMin, b);
        srMax = std::max (srMax, b);
    }
    ok = check (crossings == 5 && std::fabs ( syncPhase - 0.25 ) < 1e-9 && std::fabs (atQuarter - 1.0) < 1e-3
                    && inRange && shMin < -0.5 && shMax > 0.5 && srMin < -0.5 && srMax > 0.5,
                "M13 LFO rate & range: 5 Hz runs %d cycles/s, scripted beat 3.25 -> phase %.2f (val %.3f), "
                "S&H [%.2f, %.2f] and smooth random [%.2f, %.2f] stay in [-1, 1]",
                crossings, syncPhase, atQuarter, shMin, shMax, srMin, srMax)
       && ok;

    // Retrigger vs free-running across two voices in PatchPlayer:
    // LFO 1 is free-running (synced 1/4, saw up) -> both voices agree;
    // LFO 2 is retriggered per note (free 4 Hz, saw up) -> two staggered voices have distinct phases.
    Patch p = builtin ("pluck");
    ModLfoSpec l1, l2;
    l1.shape = LfoShape::SawUp;
    l1.sync = true;
    l1.syncDiv = LfoSyncDiv::Div1_4;
    l1.retrigger = false;
    l2.shape = LfoShape::SawUp;
    l2.sync = false;
    l2.rateHz = 3.0;
    l2.retrigger = true;
    setPatchModLfo (p, 0, l1);
    setPatchModLfo (p, 1, l2);
    setPatchMacroMod (p, ModSource::Lfo1, 1, 0.5);
    setPatchMacroMod (p, ModSource::Lfo2, 2, 0.5);

    OverrideTable table;
    applyPatchTweaks (p, table);
    std::string error;
    auto prog = PatchProgram::build (p, kRate, error);
    float targets[PatchPlayer::kNumMacros] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
    PatchPlayer player;
    player.setProgram (prog.get());
    player.prepare (kRate, 256, targets);
    player.setHostTransport (120.0, true, true, 8.0);
    player.setVoiceControls (PatchPlayer::VoiceMode::Poly, 0.0);
    player.setNodeHooks (&table, nullptr);

    static const uint8_t n1[3] = { 0x90, 48, 100 };
    static const uint8_t n2[3] = { 0x90, 55, 100 };
    const std::vector<Ev> evs = { { 0, n1, 3 }, { 6000, n2, 3 } };
    auto it = evs.cbegin();
    for (int pos = 0; pos < 7000; pos += 256)
    {
        const int chunk = std::min (256, 7000 - pos);
        player.setHostTransport (120.0, true, true, 8.0 + (pos / kRate) * 2.0);
        player.renderChunk (chunk, targets, it, evs.cend(), pos);
    }
    int slotA = -1, slotB = -1;
    for (int s = 0; s < PatchPlayer::kNumVoiceObjects; ++s)
    {
        if (player.slotNote (s) == 48)
            slotA = s;
        else if (player.slotNote (s) == 55)
            slotB = s;
    }
    const double freeDiff = (slotA >= 0 && slotB >= 0)
                              ? std::fabs (player.slotModValue (slotA, ModSource::Lfo1)
                                           - player.slotModValue (slotB, ModSource::Lfo1))
                              : 999.0;
    const double retrigDiff = (slotA >= 0 && slotB >= 0)
                                ? std::fabs (player.slotModValue (slotA, ModSource::Lfo2)
                                             - player.slotModValue (slotB, ModSource::Lfo2))
                                : 0.0;
    ok = check (slotA >= 0 && slotB >= 0 && freeDiff < 1e-4 && retrigDiff > 0.3,
                "M13 LFO retrigger vs free: two staggered voices agree on free-running LFO 1 (|diff| %.2e) and "
                "differ on retriggered LFO 2 (|diff| %.3f)",
                freeDiff, retrigDiff)
       && ok;

    return ok;
}

//==============================================================================
// 2. ADSR envelope stages.
bool smokeAdsrStages()
{
    AdsrVoiceState env;
    env.prepare (kRate);
    ModEnvSpec spec;
    spec.attack = 0.01;
    spec.decay = 0.02;
    spec.sustain = 0.40;
    spec.release = 0.02;

    double ph = 0.0;
    const bool idleBefore = env.stage() == AdsrVoiceState::Stage::Idle && env.step (spec, 0.0, ph) == 0.0;

    env.noteOn();
    const bool startsAttack = env.stage() == AdsrVoiceState::Stage::Attack;
    bool sawDecay = false, sawSustain = false;
    double peakVal = 0.0, sustainVal = 0.0;
    for (int i = 0; i < 6000; ++i)
    {
        const double v = env.step (spec, 1.0, ph);
        peakVal = std::max (peakVal, v);
        if (env.stage() == AdsrVoiceState::Stage::Decay)
            sawDecay = true;
        if (env.stage() == AdsrVoiceState::Stage::Sustain)
            sawSustain = true;
        sustainVal = v;
    }
    double relStart = sustainVal, relEnd = sustainVal;
    bool sawRelease = false, sawIdleAfter = false;
    for (int i = 0; i < 6000; ++i)
    {
        const double v = env.step (spec, 0.0, ph);
        if (env.stage() == AdsrVoiceState::Stage::Release)
            sawRelease = true;
        if (env.stage() == AdsrVoiceState::Stage::Idle)
            sawIdleAfter = true;
        relEnd = v;
    }

    return check (idleBefore && startsAttack && sawDecay && sawSustain && sawRelease && sawIdleAfter
                      && peakVal > 0.95 && std::fabs (sustainVal - 0.40) < 0.02 && relStart > 0.38 && relEnd < 1e-3,
                  "M13 ADSR stages: Idle -> Attack (peak %.3f) -> Decay -> Sustain (%.3f, want 0.40) -> Release -> "
                  "Idle (%.4f)",
                  peakVal, sustainVal, relEnd);
}

//==============================================================================
// 3. Macro modulation is per voice (two voices at different envelope stages
// read different m-values; master VM sees the unmodulated macro).
bool smokePerVoiceMacroModulation()
{
    Patch p = builtin ("pluck");
    p.voiceMode = "poly";
    // Voice writes its own m1 (signed by note so both voices can be separated),
    // while master writes its own unmodulated m1 into outR.
    p.init = "0;";
    p.voice = "out = (note == 48 ? 1 : -1) * m1 * 0.25;";
    p.master = "outl = inl; outr = m1 * 0.25;";
    p.trim = 1.0;

    ModEnvSpec e1;
    e1.attack = 0.15;
    e1.decay = 0.20;
    e1.sustain = 1.0;
    e1.release = 0.20;
    setPatchModEnv (p, 0, e1);
    setPatchMacroMod (p, ModSource::Env1, 1, 0.60);

    OverrideTable table;
    applyPatchTweaks (p, table);
    std::string error;
    auto prog = PatchProgram::build (p, kRate, error, 1);
    if (! check (prog != nullptr, "M13 per-voice macro test patch builds: %s", error.c_str()))
        return false;

    float targets[PatchPlayer::kNumMacros] = { 0.20f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
    PatchPlayer player;
    player.setProgram (prog.get());
    player.prepare (kRate, 256, targets);
    player.setVoiceControls (PatchPlayer::VoiceMode::Poly, 0.0);
    player.setNodeHooks (&table, nullptr);

    static const uint8_t n48[3] = { 0x90, 48, 100 };
    static const uint8_t n60[3] = { 0x90, 60, 100 };
    // Trigger note 48 at sample 0, and note 60 at sample 4800 (0.1 s into the 0.15 s attack).
    const std::vector<Ev> evs = { { 0, n48, 3 }, { 4800, n60, 3 } };
    auto it = evs.cbegin();
    for (int pos = 0; pos < 5120; pos += 256)
        player.renderChunk (256, targets, it, evs.cend(), pos);

    int slot48 = -1, slot60 = -1;
    for (int s = 0; s < PatchPlayer::kNumVoiceObjects; ++s)
    {
        if (player.slotNote (s) == 48)
            slot48 = s;
        else if (player.slotNote (s) == 60)
            slot60 = s;
    }
    const double m1Voice48 = slot48 >= 0 ? player.slotModulatedMacro (slot48, 0) : 0.0;
    const double m1Voice60 = slot60 >= 0 ? player.slotModulatedMacro (slot60, 0) : 0.0;

    // Master VM reads unmodulated m1 = 0.20 -> 0.05 before the DC blocker, while
    // the two voices at different envelope stages read different m1 values.
    return check (slot48 >= 0 && slot60 >= 0 && m1Voice48 > 0.55 && m1Voice60 > 0.20 && m1Voice60 < 0.35
                      && (m1Voice48 - m1Voice60) > 0.25,
                  "M13 per-voice macro modulation: voice A (100 ms into attack) reads m1=%.3f, voice B (just "
                  "triggered) reads m1=%.3f (base 0.20 + Env1*0.60)",
                  m1Voice48, m1Voice60);
}

//==============================================================================
// 4. Inspector knob modulation + tweak clamping in normalized and semitone space.
bool smokeKnobModAndTweakClamping()
{
    bool ok = true;

    const LexiconEntry* flt = findLexiconEntry ("flt_lp");
    const LexiconEntry* saw = findLexiconEntry ("osc_saw");
    if (! check (flt != nullptr && saw != nullptr, "M13 lexicon lookup for flt_lp and osc_saw"))
        return false;
    const LexiconArg& cutoffMeta = flt->args[2]; // 0..1 normalized
    const LexiconArg& freqMeta = saw->args[1];   // 20..20000 Hz

    // Direct modulateArg checks: normalized space + clamping at [0, 1].
    const double normMid = modulateArg (cutoffMeta, 0.5, 0.3);
    const double normClampHi = modulateArg (cutoffMeta, 0.85, 0.5);
    const double normClampLo = modulateArg (cutoffMeta, 0.15, -0.5);

    // Semitone space (±24 semitones = ±2 octaves at depth ±1, so +0.5 is +12 st = 2x Hz)
    // + clamping at [20, 20000].
    const double hzUpOct = modulateArg (freqMeta, 220.0, 0.5); // +12 st = 440 Hz
    const double hzClampHi = modulateArg (freqMeta, 8000.0, 1.0);
    const double hzClampLo = modulateArg (freqMeta, 50.0, -1.0);

    ok = check (std::fabs (normMid - 0.8) < 1e-12 && normClampHi == 1.0 && normClampLo == 0.0
                    && std::fabs (hzUpOct - 440.0) < 1e-6 && hzClampHi == 20000.0 && hzClampLo == 20.0,
                "M13 knob modulation math: normalized 0.5+0.3=%.2f (clamps %.1f/%.1f), Hz +12st 220->%.1f Hz "
                "(clamps %.0f/%.0f Hz)",
                normMid, normClampLo, normClampHi, hzUpOct, hzClampLo, hzClampHi)
       && ok;

    // Through LexiconHooks + OverrideTable: combine an M10 Offset tweak (+0.35 on cutoff 0.5 -> 0.85)
    // with an LFO modulation (+0.5 depth -> clamps at 1.0; -0.5 depth -> 0.35).
    Patch p = builtin ("reese");
    setPatchArgTweak (p, "flt_lp:0", "cutoff", ArgTweak::kOffset, 0.35);
    ModLfoSpec sq;
    sq.shape = LfoShape::Square;
    sq.sync = false;
    sq.rateHz = 4.0;
    sq.retrigger = true;
    setPatchModLfo (p, 0, sq);
    setPatchArgMod (p, ModSource::Lfo1, "flt_lp:0", "cutoff", 0.50);

    OverrideTable table;
    applyPatchTweaks (p, table);
    const auto& ov = table.nodes[(int) RackKind::Voice][(int) ModuleId::FltLp][0].args[2];
    const double tweaked = overrideArg (cutoffMeta, static_cast<ArgMode> (ov.mode.load()), ov.applied.load(),
                                        ov.mapLo.load(), ov.mapHi.load(), 0.5, 0.5);
    const double modPlus = modulateArg (cutoffMeta, tweaked, 0.50 * 1.0);
    const double modMinus = modulateArg (cutoffMeta, tweaked, 0.50 * -1.0);
    const std::vector<float> audio = renderPhrase (p, &table, 0.6);
    bool finite = ! audio.empty();
    for (float s : audio)
        finite = finite && isFiniteFloat (s);

    ok = check (std::fabs (tweaked - 0.85) < 1e-9 && modPlus == 1.0 && std::fabs (modMinus - 0.35) < 1e-9 && finite,
                "M13 knob modulation + tweak: cutoff 0.5 + offset 0.35 = %.2f, +0.5 mod clamps to %.2f, -0.5 mod -> "
                "%.2f; hooked render finite",
                tweaked, modPlus, modMinus)
       && ok;

    return ok;
}

//==============================================================================
// 5. With no modulation assigned, renders are bit-identical across all 15 built-ins.
bool smokeNoModBitIdentical()
{
    int identical = 0;
    std::string bad;
    for (int i = 0; i < numBuiltinPresets(); ++i)
    {
        const Patch patch = parsePatch (builtinPresetText (i)).patch;
        OverrideTable emptyTable;
        const int count = applyPatchTweaks (patch, emptyTable);
        const std::vector<float> baseline = renderPhrase (patch, nullptr, 0.8);
        const std::vector<float> withTable = renderPhrase (patch, &emptyTable, 0.8);
        if (count == 0 && ! emptyTable.anyMods() && ! baseline.empty() && sameFloatBits (baseline, withTable))
            ++identical;
        else
            bad += " " + std::string (builtinPresetFile (i));
    }
    return check (identical == numBuiltinPresets(),
                  "M13 no-mod bit-identical: %d/%d built-in presets render bit-identically with an unmodulated "
                  "override table%s",
                  identical, numBuiltinPresets(), bad.c_str());
}

//==============================================================================
// 6. Persistence (.pulsar round-trip, version 1, older files without "mods",
// --bake leaves mods untouched and notes it, --render plays them).
bool smokeRoundTripAndBake()
{
    bool ok = true;

    // Older files without "mods" load with empty mods and serialize without a "mods" key.
    const Patch plain = builtin ("reese");
    const std::string plainText = serializePatch (plain);
    ok = check (plain.mods.empty() && plainText.find ("\"mods\"") == std::string::npos,
                "M13 persistence: unmodulated patches omit the optional \"mods\" key")
       && ok;

    // Configure all 4 modulators and 4 assignments on Reese, round-trip through .pulsar JSON.
    Patch modReese = plain;
    ModLfoSpec lfo1, lfo2;
    lfo1.shape = LfoShape::SmoothRandom;
    lfo1.sync = true;
    lfo1.syncDiv = LfoSyncDiv::Div1_8D;
    lfo1.retrigger = false;
    lfo2.shape = LfoShape::SampleAndHold;
    lfo2.sync = false;
    lfo2.rateHz = 7.25;
    lfo2.retrigger = true;
    ModEnvSpec env1, env2;
    env1.attack = 0.025;
    env1.decay = 0.31;
    env1.sustain = 0.42;
    env1.release = 0.55;
    env2.attack = 0.12;
    env2.decay = 0.45;
    env2.sustain = 0.20;
    env2.release = 0.80;
    setPatchModLfo (modReese, 0, lfo1);
    setPatchModLfo (modReese, 1, lfo2);
    setPatchModEnv (modReese, 0, env1);
    setPatchModEnv (modReese, 1, env2);
    setPatchMacroMod (modReese, ModSource::Lfo1, 2, 0.45);
    setPatchMacroMod (modReese, ModSource::Env1, 4, -0.35);
    setPatchArgMod (modReese, ModSource::Lfo2, "flt_lp:0", "cutoff", 0.50);
    setPatchArgMod (modReese, ModSource::Env2, "flt_lp:0", "res", -0.25);

    const std::string json = serializePatch (modReese);
    const ParseResult pr = parsePatch (json);
    ok = check (pr.ok() && json.find ("\"version\": 1") != std::string::npos
                    && json.find ("\"mods\"") != std::string::npos && pr.patch == modReese
                    && pr.patch.mods.assignments.size() == 4
                    && pr.patch.mods.lfos[0].shape == LfoShape::SmoothRandom
                    && pr.patch.mods.lfos[0].syncDiv == LfoSyncDiv::Div1_8D
                    && pr.patch.mods.lfos[1].shape == LfoShape::SampleAndHold && ! pr.patch.mods.lfos[1].sync,
                "M13 persistence: \"mods\" (2 LFOs, 2 envs, 4 assignments) round-trips through .pulsar JSON with "
                "version 1")
       && ok;

    // --render plays mods (audio differs from unmodulated Reese).
    OverrideTable modTable;
    const int applied = applyPatchTweaks (pr.patch, modTable);
    const std::vector<float> dryAudio = renderPhrase (plain, nullptr, 0.8);
    const std::vector<float> modAudio = renderPhrase (pr.patch, &modTable, 0.8);
    ok = check (applied == 4 && modTable.anyMods() && ! sameFloatBits (dryAudio, modAudio),
                "M13 render: applyPatchTweaks activates %d mod assignments and changes the rendered audio", applied)
       && ok;

    // --bake: bakes tweaks into code, leaves mods untouched and reports a note.
    Patch withTweakAndMod = modReese;
    setPatchArgTweak (withTweakAndMod, "flt_lp:0", "cutoff", ArgTweak::kOffset, 0.15);
    const BakeResult baked = bakePatch (withTweakAndMod);
    bool notedMods = false;
    for (const auto& n : baked.notes)
        if (n.find ("mods left untouched") != std::string::npos)
            notedMods = true;
    ok = check (baked.ok && baked.patch.tweaks.empty() && baked.patch.mods == modReese.mods && notedMods,
                "M13 bake: --bake writes knob tweaks into code, leaves mods untouched (%d assignments kept) and "
                "notes it",
                (int) baked.patch.mods.assignments.size())
       && ok;

    return ok;
}

//==============================================================================
// 7. Undo, Discard / Original card, Change this node, Mutate, and Generate rules.
bool smokeUndoDiscardChangeMutate()
{
    bool ok = true;

    Patch base = builtin ("reese");
    ModLfoSpec lfo1;
    lfo1.shape = LfoShape::Triangle;
    lfo1.sync = true;
    lfo1.syncDiv = LfoSyncDiv::Div1_8;
    setPatchModLfo (base, 0, lfo1);
    setPatchMacroMod (base, ModSource::Lfo1, 1, 0.40);
    setPatchArgMod (base, ModSource::Env1, "flt_lp:0", "cutoff", 0.35);
    setPatchArgMod (base, ModSource::Lfo2, "fx_fold#0", "amount", 0.50);

    // Undo and Discard / Original card restore mods bit-for-bit.
    PatchSnapshot snap;
    snap.patch = base;
    snap.macros.fill (0.4f);
    UndoStack stack;
    stack.push (snap);
    PatchSnapshot popped;
    const bool gotUndo = stack.pop (popped);
    const ParseResult reloaded = parsePatch (serializePatch (popped.patch));
    ok = check (gotUndo && popped.sameAs (snap) && reloaded.ok() && reloaded.patch.mods == base.mods,
                "M13 undo & discard: PatchSnapshot and .pulsar reload restore modulator settings and %d assignments "
                "bit-for-bit",
                (int) reloaded.patch.mods.assignments.size())
       && ok;

    // Change this node: keeps macro modulation and surviving node-knob modulation,
    // drops modulation on removed nodes (fx_fold#0) and names it in droppedTweaks.
    Patch replyPatch = builtin ("reese");
    replyPatch.voice = replaced (replyPatch.voice, "y = fx_fold(y, m4 * 0.35);\n", "");
    GenerationRequest gr;
    gr.kind = GenerationKind::Change;
    gr.base = base;
    gr.node = "flt_lp:0";
    gr.instruction = "drop the fold";
    gr.measureVoice = [] (const Patch& p) { return p.voiceBudgetNs() * 0.1; };
    gr.transport = [&] (const ChatRequest&)
    {
        ChatReply r;
        r.content = formatPatchAsReply (replyPatch);
        return r;
    };
    const GenerationResult chRes = Generator::run (gr, nullptr);
    const ModAssignment* keptMacro = findMacroMod (chRes.patch, 1);
    const ModAssignment* keptFlt = findArgMod (chRes.patch, "flt_lp:0", "cutoff");
    const ModAssignment* droppedFold = findArgMod (chRes.patch, "fx_fold#0", "amount");
    std::string droppedList;
    for (const auto& d : chRes.droppedTweaks)
        droppedList += (droppedList.empty() ? "" : "; ") + d;
    ok = check (chRes.ok && keptMacro != nullptr && sameBits (keptMacro->depth, 0.40) && keptFlt != nullptr
                    && sameBits (keptFlt->depth, 0.35) && droppedFold == nullptr
                    && chRes.patch.mods.assignments.size() == 2 && chRes.droppedTweaks.size() == 1
                    && droppedList == "LFO 2 -> fx_fold#0 amount",
                "M13 change: keeps M1 and surviving flt_lp:0 mod, drops removed node mod and names it (\"%s\")",
                droppedList.c_str())
       && ok;

    // Mutate: keeps macro modulation (and modulator settings) and drops node-knob modulation.
    const MutateBase mb = prepareMutateBase (base);
    MutateRequest mr;
    mr.base = mb.patch;
    mr.baseNodesKnown = false;
    mr.measureVoice = [] (const Patch& p) { return p.voiceBudgetNs() * 0.1; };
    mr.backoffSeconds = 0.01;
    mr.transport = [&] (const ChatRequest&, const std::atomic<bool>*)
    {
        ChatReply r;
        r.content = formatPatchAsReply (replyPatch);
        return r;
    };
    const MutateResult mutRes = Mutator::run (mr, nullptr);
    int mutKeptMacro = 0;
    for (const auto& slot : mutRes.slots)
        if (slot.ok && slot.patch.mods.assignments.size() == 1 && findMacroMod (slot.patch, 1) != nullptr
            && findArgMod (slot.patch, "flt_lp:0", "cutoff") == nullptr
            && slot.patch.mods.lfos[0].shape == LfoShape::Triangle)
            ++mutKeptMacro;
    ok = check (mb.patch.mods.assignments.size() == 1 && mutRes.passed() == 4 && mutKeptMacro == 4,
                "M13 mutate: keeps macro modulation (M1) and modulator settings across all %d/4 variations and drops "
                "node-knob modulation",
                mutKeptMacro)
       && ok;

    // Generate: clears all mods.
    GenerationRequest genReq = gr;
    genReq.kind = GenerationKind::Generate;
    genReq.prompt = "reese";
    const GenerationResult genRes = Generator::run (genReq, nullptr);
    ok = check (genRes.ok && genRes.patch.mods.empty(), "M13 generate: a newly generated patch starts with no mods")
       && ok;

    return ok;
}

//==============================================================================
// 8. Bounds safety: extreme depths, rates, and envelope times never produce NaN/Inf.
bool smokeBoundsNoNaN()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    VoiceModPack pack;
    pack.prepare (kRate, 7);
    pack.noteOn (45);

    std::array<ModLfoSpec, kNumModLfos> lfos;
    lfos[0].shape = LfoShape::SmoothRandom;
    lfos[0].sync = false;
    lfos[0].rateHz = 1e9;
    lfos[1].shape = LfoShape::Sine;
    lfos[1].sync = false;
    lfos[1].rateHz = nan;

    std::array<ModEnvSpec, kNumModEnvs> envs;
    envs[0].attack = 0.0;
    envs[0].decay = -10.0;
    envs[0].sustain = 999.0;
    envs[0].release = inf;
    envs[1].attack = nan;
    envs[1].decay = 1e9;
    envs[1].sustain = nan;
    envs[1].release = 0.0;

    bool allFinite = true;
    for (int i = 0; i < 4000; ++i)
    {
        pack.stepAll (lfos, envs, i % 2 == 0 ? nan : 1e9, i % 3 == 0 ? -1e9 : nan, i < 2000 ? 1.0 : 0.0);
        for (int s = 0; s < kNumModSources; ++s)
            if (! isFiniteDouble (pack.values[(size_t) s]) || ! isFiniteDouble (pack.phases[(size_t) s])
                || pack.values[(size_t) s] < -1.0001 || pack.values[(size_t) s] > 1.0001)
                allFinite = false;
    }

    const LexiconEntry* saw = findLexiconEntry ("osc_saw");
    const LexiconEntry* flt = findLexiconEntry ("flt_lp");
    const double m1 = modulateArg (saw->args[1], 440.0, 1e9 * (-1e9));
    const double m2 = modulateArg (saw->args[1], 440.0, nan * inf);
    const double m3 = modulateArg (flt->args[2], 0.5, -1e9 * 1e9);
    const double m4 = modulateArg (flt->args[2], 0.5, nan);
    allFinite = allFinite && isFiniteDouble (m1) && isFiniteDouble (m2) && isFiniteDouble (m3) && isFiniteDouble (m4)
             && m1 >= 20.0 && m1 <= 20000.0 && m3 >= 0.0 && m3 <= 1.0;

    // Also render through PatchPlayer with extreme depths/rates stored in a Patch.
    Patch p = builtin ("reese");
    p.mods.lfos[0].sync = false;
    p.mods.lfos[0].rateHz = 1e9;
    p.mods.envs[0].attack = 0.0;
    p.mods.envs[0].decay = 0.0;
    p.mods.envs[0].sustain = 10.0;
    p.mods.envs[0].release = 0.0;
    p.mods.assignments.push_back ({ ModSource::Lfo1, 1, {}, {}, 1e9 });
    p.mods.assignments.push_back ({ ModSource::Env1, 0, "flt_lp:0", "cutoff", -1e9 });
    OverrideTable table;
    applyPatchTweaks (p, table);
    const std::vector<float> audio = renderPhrase (p, &table, 0.5);
    for (float s : audio)
        allFinite = allFinite && isFiniteFloat (s);

    return check (allFinite && ! audio.empty(),
                  "M13 bounds safety: extreme rates (0, 1e9, NaN), envelope times (0, -10, Inf, NaN) and depths "
                  "(+-1e9, NaN) stay clamped and never produce NaN");
}

//==============================================================================
// Merge M12 x M13: LFO 1 on one argument of each M12 function (Tier 1 and
// Tier 2, voice and master), through the generic hook path. Each render must be finite and differ from the same
// patch without the assignment (so the modulation reached the function).
bool smokeM12FunctionsModulated()
{
    Patch base;
    base.name = "M12 x M13";
    base.voiceMode = "poly";
    base.trim = 0.75;
    base.macros = { { "A", 0.5 }, { "B", 0.5 }, { "C", 0.5 }, { "D", 0.5 },
                    { "E", 0.5 }, { "F", 0.5 }, { "G", 0.5 }, { "H", 0.5 } };
    base.init = "0;";
    base.voice =
        "l = mod_lfo(0, 1.0, 0.0);\n"
        "wt = osc_wt(0, freq, 2, 0.3);\n"
        "fm = osc_fm(0, freq, 0.25, 0.3, 0.1);\n"
        "sy = osc_sync(0, freq, 0.4, 0.5);\n"
        "s = fx_shape(wt * 0.4 + fm * 0.3 + sy * 0.3, 0.4, 0.3);\n"
        "s = flt_formant(0, s, 0.4, 0.4);\n"
        "s = flt_notch(0, s, 0.5, 0.3);\n"
        "s = flt_peak(0, s, 0.4, 0.6, 0.3);\n"
        "s = flt_tilt(0, s, 0.5 + l * 0.3);\n"
        "s = fx_phaser(0, s, 0.4, 0.7, 0.3);\n"
        "s = fx_fshift(0, s, 0.5);\n"
        "env = env_adsr(0, gate, 0.01, 0.3, 0.8, 0.2);\n"
        "out = s * env * vel * 0.4;";
    base.master =
        "d = bus_delay(inl, inr, 0.25, 0.4, 0.4);\n"
        "dr = bus_delay_r();\n"
        "outl = bus_width(d, dr, 0.5);\n"
        "outr = bus_width_r();";

    struct Target { const char* node; const char* arg; };
    const Target targets[] = {
        { "osc_wt:0", "pos" },      { "osc_fm:0", "index" },    { "osc_sync:0", "ratio" },
        { "fx_shape#0", "drive" },  { "flt_formant:0", "vowel" }, { "flt_notch:0", "cutoff" },
        { "flt_peak:0", "gain" },   { "flt_tilt:0", "tilt" },   { "mod_lfo:0", "beats" },
        { "fx_phaser:0", "pos" },   { "fx_fshift:0", "shift" }, { "master/bus_delay", "feedback" },
        { "master/bus_width", "amount" },
    };

    ModLfoSpec lfo;
    lfo.shape = LfoShape::Sine;
    lfo.sync = false;
    lfo.rateHz = 7.0;
    lfo.retrigger = true;

    Patch plainPatch = base;
    setPatchModLfo (plainPatch, 0, lfo);
    OverrideTable plainTable;
    applyPatchTweaks (plainPatch, plainTable);
    const std::vector<float> plain = renderPhrase (plainPatch, &plainTable, 0.6);

    int passed = 0;
    std::string failed;
    for (const auto& tg : targets)
    {
        Patch p = plainPatch;
        setPatchArgMod (p, ModSource::Lfo1, tg.node, tg.arg, 0.6);
        OverrideTable table;
        applyPatchTweaks (p, table);
        const std::vector<float> audio = renderPhrase (p, &table, 0.6);
        bool finite = ! audio.empty() && audio.size() == plain.size();
        double maxDiff = 0.0, peak = 0.0;
        for (size_t i = 0; finite && i < audio.size(); ++i)
        {
            finite = isFiniteFloat (audio[i]);
            peak = std::max (peak, (double) std::fabs (audio[i]));
            maxDiff = std::max (maxDiff, (double) std::fabs (audio[i] - plain[i]));
        }
        if (finite && peak > 1e-3 && maxDiff > 1e-4)
            ++passed;
        else
            failed += std::string (" ") + tg.node + "." + tg.arg;
    }
    return check (passed == (int) std::size (targets),
                  "M12 x M13: LFO 1 on one argument of each M12 function (osc_wt pos, osc_fm index, osc_sync ratio, "
                  "fx_shape drive, flt_formant vowel, flt_notch cutoff, flt_peak gain, flt_tilt tilt, mod_lfo beats, "
                  "fx_phaser pos, fx_fshift shift, bus_delay feedback, bus_width amount) "
                  "renders finite and changes the output: %d/%d%s",
                  passed, (int) std::size (targets), failed.empty() ? "" : (" failed:" + failed).c_str());
}

//==============================================================================
// Control-rate modulators (merge/perf pass): the runner vs the per-sample
// reference, sharp S&H, the fast exp2, and editor-open stepping only the
// MOD tiles' voice.
bool smokeControlRateMods()
{
    bool ok = true;

    // fastExp2 (Hz knob modulation) against std::exp2, in cents.
    double worstCents = 0.0;
    for (int k = -16000; k <= 16000; ++k)
    {
        const double x = k * 0.0005;  // +-8 octaves (+-96 st; the knob range is +-24 st per source)
        const double cents = 1200.0 * std::fabs (std::log2 (fastExp2 (x) / std::exp2 (x)));
        worstCents = std::max (worstCents, cents);
    }
    ok = check (worstCents < 0.1, "M13 fast exp2 for Hz modulation: worst error %.4f cents over +-8 octaves "
                                  "(limit 0.1)", worstCents)
       && ok;

    // Runner vs per-sample reference at 96 kHz: sine LFO, synced triangle,
    // two ADSRs with a gate-off at 0.6 s.
    constexpr double rate = 96000.0;
    ModSpecs sp;
    sp.lfos[0].shape = LfoShape::Sine;
    sp.lfos[0].sync = false;
    sp.lfos[0].rateHz = 3.0;
    sp.lfos[0].retrigger = true;
    sp.lfos[1].shape = LfoShape::Triangle;
    sp.lfos[1].sync = true;
    sp.lfos[1].syncDiv = LfoSyncDiv::Div1_8;
    sp.lfos[1].retrigger = false;
    sp.envs[0] = { 0.05, 0.1, 0.6, 0.2 };
    sp.envs[1] = { 0.02, 0.3, 0.2, 0.05 };
    VoiceModPack ref, run;
    ref.prepare (rate, 3);
    run.prepare (rate, 3);
    ref.noteOn (45);
    run.noteOn (45);
    const double bpm = 128.0;
    const double step = bpm / 60.0 / rate;
    double worst[kNumModSources] = {};
    int moved = 0, refMoved = 0;
    double prev0 = 0.0;
    const int total = static_cast<int> (rate * 1.0);
    for (int i = 0; i < total; ++i)
    {
        const double gate = i < static_cast<int> (rate * 0.6) ? 1.0 : 0.0;
        const double beat = i * step;
        const double ref0 = ref.values[0];
        ref.stepAll (sp.lfos, sp.envs, bpm, beat, gate);
        run.tick (0x0F, sp, bpm, beat, step, gate);
        for (int s = 0; s < kNumModSources; ++s)
            worst[s] = std::max (worst[s], std::fabs (ref.values[(size_t) s] - run.values[(size_t) s]));
        if (i > 0)
        {
            moved += run.values[0] != prev0 ? 1 : 0;
            refMoved += ref.values[0] != ref0 ? 1 : 0;
        }
        prev0 = run.values[0];
    }
    const double worstAll = std::max (std::max (worst[0], worst[1]), std::max (worst[2], worst[3]));
    ok = check (worstAll < 0.02 && moved >= refMoved - 2,
                "M13 control-rate modulators (every %d samples, linear ramp, 0.5 ms one-pole per block) track the "
                "per-sample reference: worst |diff| LFO %.4f / %.4f, env %.4f / %.4f (limit 0.02); the output moves "
                "on %d of %d samples where the reference moves (no steps)",
                kModControlBlock, worst[0], worst[1], worst[2], worst[3], moved, refMoved)
       && ok;

    // S&H: constant within a cycle, no change before the cycle boundary,
    // and 90 % of the jump within 2 ms after it (the one-pole alone: 1.15 ms).
    ModSpecs sh;
    sh.lfos[0].shape = LfoShape::SampleAndHold;
    sh.lfos[0].sync = false;
    sh.lfos[0].rateHz = 50.0;  // 1920-sample cycles at 96 kHz
    sh.lfos[0].retrigger = true;
    VoiceModPack shRun;
    shRun.prepare (rate, 4);
    shRun.noteOn (52);
    std::vector<double> out;
    for (int i = 0; i < 9600; ++i)
    {
        shRun.tick (0x01, sh, bpm, i * step, step, 1.0);
        out.push_back (shRun.values[0]);
    }
    const int cycle = 1920;
    int early = 0, slow = 0, jumps = 0;
    for (int c = 1; c < 5; ++c)
    {
        const int b = c * cycle;
        const double before = out[(size_t) (b - 1)];
        const double settled = out[(size_t) (b + 1800)];
        if (std::fabs (settled - before) < 1e-3)
            continue;  // the random values happened to match
        ++jumps;
        // Nothing moves in the last 600 samples of the old cycle (the
        // previous jump's 0.5 ms tail has settled to ~1e-12 by then).
        for (int i = b - 600; i < b; ++i)
            if (std::fabs (out[(size_t) i] - before) > 1e-9)
            {
                ++early;
                break;
            }
        const double target = before + 0.9 * (settled - before);
        const int limit = b + static_cast<int> (0.002 * rate);
        const bool reached = settled > before ? out[(size_t) limit] >= target : out[(size_t) limit] <= target;
        slow += reached ? 0 : 1;
    }
    ok = check (jumps >= 3 && early == 0 && slow == 0,
                "M13 S&H stays sharp at control rate: %d jumps, none starts before its cycle boundary (%d early), "
                "each reaches 90%% within 2 ms (%d slow)",
                jumps, early, slow)
       && ok;

    // Editor open, no mods: only the newest voice (the MOD tiles' voice)
    // steps its modulators; the others are left untouched.
    {
        Patch p = builtin ("pluck");
        OverrideTable table;
        applyPatchTweaks (p, table);
        auto bank = std::make_unique<TraceBank>();
        bank->enabled.store (true);
        std::string error;
        auto prog = PatchProgram::build (p, kRate, error);
        float targets[PatchPlayer::kNumMacros] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
        PatchPlayer player;
        player.setProgram (prog.get());
        player.prepare (kRate, 256, targets);
        player.setHostTransport (120.0, false, false, 0.0);
        player.setVoiceControls (PatchPlayer::VoiceMode::Poly, 0.0);
        player.setNodeHooks (&table, bank.get());
        static const uint8_t a[3] = { 0x90, 48, 100 }, b[3] = { 0x90, 52, 100 }, c[3] = { 0x90, 55, 100 };
        const std::vector<Ev> evs = { { 0, a, 3 }, { 2000, b, 3 }, { 4000, c, 3 } };
        auto it = evs.cbegin();
        auto renderTo = [&] (int from, int to)
        {
            for (int pos = from; pos < to; pos += 256)
                player.renderChunk (std::min (256, to - pos), targets, it, evs.cend(), pos);
        };
        auto snapshot = [&] (std::vector<double>& v)
        {
            v.clear();
            for (int s = 0; s < PatchPlayer::kNumVoiceObjects; ++s)
                for (int src = 0; src < kNumModSources; ++src)
                    v.push_back (player.slotModValue (s, static_cast<ModSource> (src)));
        };
        // Note 55 (the newest) starts at 4000; snapshot after it, then again later.
        renderTo (0, 4608);
        std::vector<double> early, late;
        snapshot (early);
        renderTo (4608, 12032);
        snapshot (late);
        int newest = -1, frozen = 0, older = 0;
        for (int s = 0; s < PatchPlayer::kNumVoiceObjects; ++s)
        {
            if (player.slotNote (s) == 55)
                newest = s;
            else if (player.slotNote (s) >= 0)
            {
                ++older;
                bool same = true;
                for (int src = 0; src < kNumModSources; ++src)
                    same = same && sameBits (early[(size_t) (s * kNumModSources + src)],
                                             late[(size_t) (s * kNumModSources + src)]);
                frozen += same ? 1 : 0;
            }
        }
        const double newestMove = newest >= 0
            ? std::fabs (late[(size_t) (newest * kNumModSources + 2)] - early[(size_t) (newest * kNumModSources + 2)])
                  + std::fabs (late[(size_t) (newest * kNumModSources)] - early[(size_t) (newest * kNumModSources)])
            : 0.0;
        ok = check (newest >= 0 && older == 2 && frozen == older && newestMove > 0.01 && ! table.anyMods(),
                    "M13 editor open, no mods: only the newest voice (the MOD tiles') steps its modulators: it "
                    "moved by %.3f while %d/%d older voices stayed frozen",
                    newestMove, frozen, older)
           && ok;
    }

    return ok;
}

} // namespace

bool smokeM13()
{
    bool ok = true;
    ok = smokeLfoShapesAndRates() && ok;
    ok = smokeAdsrStages() && ok;
    ok = smokePerVoiceMacroModulation() && ok;
    ok = smokeKnobModAndTweakClamping() && ok;
    ok = smokeNoModBitIdentical() && ok;
    ok = smokeRoundTripAndBake() && ok;
    ok = smokeUndoDiscardChangeMutate() && ok;
    ok = smokeBoundsNoNaN() && ok;
    ok = smokeM12FunctionsModulated() && ok;
    ok = smokeControlRateMods() && ok;
    return ok;
}

} // namespace vibesynth_cli
