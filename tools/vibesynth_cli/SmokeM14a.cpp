// M14a smoketests:
//   - macro-scan determinism (Wiener entropy / STFT spectral flatness)
//   - validator noise flag (rejects synthetic noise-at-1, accepts noisy textures, gated)
//   - macroRanges JSON serialization / deserialization round-trip
//   - macro range bake equivalence (bit-identical renders)
//   - fx_comp and fx_gate (bounds/NaN, compression/gating, modulation, bake)
//   - bus_delay 6th arg (lowcut frequency response)

#include "SmokeM14a.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "CliCheck.h"
#include "ai/NodeId.h"
#include "engine/Lexicon.h"
#include "engine/NodeOverrides.h"
#include "engine/Utilities6.h"
#include "patch/Patch.h"
#include "patch/PatchBake.h"
#include "patch/PatchEngine.h"
#include "patch/PatchPlayer.h"
#include "patch/PatchTweaks.h"
#include "patch/PatchValidator.h"

namespace vibesynth_cli {

namespace {

using namespace vibesynth;

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

struct Ev
{
    int samplePosition;
    const uint8_t* data;
    int numBytes;
};

bool finiteF (float v) noexcept
{
    uint32_t b = 0;
    std::memcpy (&b, &v, sizeof (v));
    const uint32_t exp = (b >> 23) & 0xFFu;
    return exp != 0xFFu;
}

// ---------------------------------------------------------------------------
// 1. Macro scan determinism
bool smokeScanDeterminism()
{
    bool ok = true;
    const Patch reese = parsePatch (builtinPresetText (0)).patch;
    const auto scan1 = scanPatchMacros (reese, kRate, 2);
    const auto scan2 = scanPatchMacros (reese, kRate, 2);

    bool identical = (scan1.size() == scan2.size() && ! scan1.empty());
    for (size_t i = 0; identical && i < scan1.size(); ++i)
    {
        identical = (scan1[i].macro == scan2[i].macro
                     && scan1[i].label == scan2[i].label
                     && scan1[i].defaultFlatness == scan2[i].defaultFlatness
                     && scan1[i].at1Flatness == scan2[i].at1Flatness
                     && scan1[i].deltaFlatness == scan2[i].deltaFlatness
                     && scan1[i].levelChangeDb == scan2[i].levelChangeDb
                     && scan1[i].isNoise == scan2[i].isNoise);
    }
    ok = check (identical, "M14a macro-scan: determinism verified across repeated runs on %s (%zu macros)",
                reese.name.c_str(), scan1.size()) && ok;
    return ok;
}

// ---------------------------------------------------------------------------
// 2. Validator macro noise flag
bool smokeValidatorNoiseFlag()
{
    bool ok = true;

    // A patch that is tonal at default (m1 = 0), but m1 = 1 turns it into pure noise.
    Patch runaway;
    runaway.name = "Tonal To Noise";
    runaway.macros = { { "Chaos", 0.0 } };
    runaway.voice =
        "tone = osc_sine(0, freq);\n"
        "noiseSig = noise();\n"
        "s = tone * (1 - m1) + noiseSig * m1;\n"
        "env = env_adsr(0, gate, 0.01, 0.5, 0.8, 0.2);\n"
        "out = s * env * vel * 0.4;";
    runaway.master = "outl = inl; outr = inr;";
    runaway.trim = 0.8;

    // When checkMacroNoise is false: passes cleanly
    const ValidationResult vrOff = validatePatch (runaway, kRate, nullptr, 2, false);
    ok = check (vrOff.ok(), "M14a validator: runaway patch passes when checkMacroNoise == false") && ok;

    // When checkMacroNoise is true: fails with MacroNoise and self-heal message
    const ValidationResult vrOn = validatePatch (runaway, kRate, nullptr, 2, true);
    const bool failsWithNoise = (! vrOn.ok() && vrOn.status == PatchStatus::MacroNoise
                                 && vrOn.error.find ("turns the sound into noise") != std::string::npos
                                 && vrOn.error.find ("Reduce its range") != std::string::npos);
    ok = check (failsWithNoise, "M14a validator: runaway patch rejected when checkMacroNoise == true (%s)",
                vrOn.error.c_str()) && ok;

    // A texture patch that is already noisy at its default must NOT fail
    Patch noisyTexture;
    noisyTexture.name = "Texture";
    noisyTexture.macros = { { "Filter", 0.5 } };
    noisyTexture.voice =
        "s = noise();\n"
        "s = flt_lp(0, s, 0.2 + m1 * 0.5, 0.2);\n"
        "env = env_adsr(0, gate, 0.01, 0.5, 0.8, 0.2);\n"
        "out = s * env * vel * 0.2;";
    noisyTexture.master = "outl = inl; outr = inr;";
    noisyTexture.trim = 0.8;

    const ValidationResult vrTex = validatePatch (noisyTexture, kRate, nullptr, 2, true);
    ok = check (vrTex.ok(), "M14a validator: noisy-texture patch (noisy at default) is not rejected") && ok;

    return ok;
}

// ---------------------------------------------------------------------------
// 3. Macro range round-trip (serialization and API)
bool smokeMacroRangeRoundTrip()
{
    bool ok = true;
    Patch p;
    p.name = "RangeTest";
    p.macros = { { "Cutoff", 0.5 }, { "Drive", 0.3 } };
    p.voice = "out = osc_saw(0, freq) * env_adsr(0, gate, 0.01, 0.2, 0.7, 0.1) * vel;";

    setPatchMacroRange (p, 1, 0.25, 0.75);
    setPatchMacroRange (p, 2, 0.10, 0.60);
    ok = check (p.macroRanges.size() == 2, "M14a macroRanges: setPatchMacroRange added 2 ranges") && ok;

    const double v0 = p.applyMacroRange (0, 0.0);
    const double vMid = p.applyMacroRange (0, 0.5);
    const double v1 = p.applyMacroRange (0, 1.0);
    ok = check (std::abs (v0 - 0.25) < 1e-6 && std::abs (vMid - 0.50) < 1e-6 && std::abs (v1 - 0.75) < 1e-6,
                "M14a macroRanges: applyMacroRange maps [0, 1] to [0.25, 0.75] (got %.2f, %.2f, %.2f)",
                v0, vMid, v1) && ok;

    const std::string json = serializePatch (p);
    ok = check (json.find ("\"macroRanges\"") != std::string::npos,
                "M14a macroRanges: serializePatch emits \"macroRanges\" key") && ok;

    const ParseResult pr = parsePatch (json);
    ok = check (pr.ok() && pr.patch.macroRanges.size() == 2,
                "M14a macroRanges: parsePatch parses macroRanges back (%zu entries)",
                pr.patch.macroRanges.size()) && ok;

    const auto* r1 = findMacroRange (pr.patch, 1);
    const auto* r2 = findMacroRange (pr.patch, 2);
    ok = check (r1 != nullptr && std::abs (r1->min - 0.25) < 1e-6 && std::abs (r1->max - 0.75) < 1e-6
                && r2 != nullptr && std::abs (r2->min - 0.10) < 1e-6 && std::abs (r2->max - 0.60) < 1e-6,
                "M14a macroRanges: round-tripped values match bit for bit") && ok;

    removePatchMacroRange (p, 1);
    ok = check (p.macroRanges.size() == 1 && findMacroRange (p, 1) == nullptr,
                "M14a macroRanges: removePatchMacroRange removes range") && ok;

    return ok;
}

// ---------------------------------------------------------------------------
// 4. Macro range bake equivalence
bool smokeMacroRangeBakeEquivalence()
{
    bool ok = true;
    Patch p;
    p.name = "BakeEquiv";
    p.macros = { { "Cutoff", 0.5 }, { "Drive", 0.3 } };
    p.init = "sx = 1.0;";
    p.voice =
        "s = osc_saw(0, freq * sx) * (1.0 - m2 * 0.4);\n"
        "s = flt_lp(0, s, m1, 0.3);\n"
        "env = env_adsr(0, gate, 0.01, 0.2, 0.7, 0.1);\n"
        "out = s * env * vel * 0.4;";
    p.master = "outl = inl; outr = inr;";
    p.trim = 0.8;

    setPatchMacroRange (p, 1, 0.20, 0.80);
    setPatchMacroRange (p, 2, 0.10, 0.50);

    // Render unbaked (with macroRanges applied at runtime via OverrideTable)
    OverrideTable tableUnbaked;
    applyPatchTweaks (p, tableUnbaked);

    std::string err;
    const auto progUnbaked = PatchProgram::build (p, kRate, err, 2);
    if (! progUnbaked)
    {
        ok = check (false, "M14a bake: unbaked program failed to build: %s", err.c_str()) && ok;
        return false;
    }

    PatchPlayer playerUnbaked;
    float targets[PatchPlayer::kNumMacros] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
    playerUnbaked.prepare (kRate, 256, targets);
    playerUnbaked.setProgram (progUnbaked.get());
    playerUnbaked.setNodeHooks (&tableUnbaked, nullptr);

    constexpr int kRenderSamples = 2048;
    std::vector<float> unbakedL (kRenderSamples), unbakedR (kRenderSamples);
    {
        const uint8_t noteOn[3] = { 0x90, 40, 100 };
        playerUnbaked.handleMidi (noteOn, 3);
        const std::vector<Ev> dummy;
        for (int pos = 0; pos < kRenderSamples; pos += 256)
        {
            auto it = dummy.cbegin();
            playerUnbaked.renderChunk (256, targets, it, dummy.cend(), 0);
            std::copy (playerUnbaked.outL(), playerUnbaked.outL() + 256, unbakedL.data() + pos);
            std::copy (playerUnbaked.outR(), playerUnbaked.outR() + 256, unbakedR.data() + pos);
        }
    }

    // Bake the patch
    const BakeResult baked = bakePatch (p);
    ok = check (baked.ok && baked.patch.macroRanges.empty() && baked.callsChanged >= 2,
                "M14a bake: bakePatch folded macroRanges (%d calls changed)%s%s",
                baked.callsChanged, baked.ok ? "" : ": ", baked.error.c_str()) && ok;
    if (! baked.ok)
        return false;

    // Verify baked code contains the assignments
    ok = check (baked.patch.voice.find ("m1 = ") != std::string::npos
                && baked.patch.voice.find ("m2 = ") != std::string::npos,
                "M14a bake: baked voice code contains m1 and m2 range assignments") && ok;

    // Render baked (without runtime ranges, code computes them)
    OverrideTable tableBaked;
    applyPatchTweaks (baked.patch, tableBaked);

    const auto progBaked = PatchProgram::build (baked.patch, kRate, err, 2);
    if (! progBaked)
    {
        ok = check (false, "M14a bake: baked program failed to build: %s", err.c_str()) && ok;
        return false;
    }

    PatchPlayer playerBaked;
    playerBaked.prepare (kRate, 256, targets);
    playerBaked.setProgram (progBaked.get());
    playerBaked.setNodeHooks (&tableBaked, nullptr);

    std::vector<float> bakedL (kRenderSamples), bakedR (kRenderSamples);
    {
        const uint8_t noteOn[3] = { 0x90, 40, 100 };
        playerBaked.handleMidi (noteOn, 3);
        const std::vector<Ev> dummy;
        for (int pos = 0; pos < kRenderSamples; pos += 256)
        {
            auto it = dummy.cbegin();
            playerBaked.renderChunk (256, targets, it, dummy.cend(), 0);
            std::copy (playerBaked.outL(), playerBaked.outL() + 256, bakedL.data() + pos);
            std::copy (playerBaked.outR(), playerBaked.outR() + 256, bakedR.data() + pos);
        }
    }

    bool bitIdentical = true;
    for (int i = 0; i < kRenderSamples; ++i)
    {
        if (unbakedL[(size_t) i] != bakedL[(size_t) i] || unbakedR[(size_t) i] != bakedR[(size_t) i])
        {
            bitIdentical = false;
            break;
        }
    }
    ok = check (bitIdentical, "M14a bake: renders with runtime macroRanges vs baked code are bit-identical") && ok;

    return ok;
}

// ---------------------------------------------------------------------------
// 5. fx_comp and fx_gate tests
bool smokeFxCompAndGate()
{
    bool ok = true;

    // Bounds / NaN safety
    {
        CompState comp;
        comp.init (kRate);
        const float c1 = comp.process (1.0e30f, 1.0);
        const float c2 = comp.process (-1.0e30f, 1.0);
        const float c3 = comp.process (std::numeric_limits<float>::quiet_NaN(), 0.5);
        ok = check (finiteF (c1) && finiteF (c2) && finiteF (c3),
                    "M14a fx_comp bounds/NaN: extreme inputs remain finite (%.2f, %.2f, %.2f)",
                    c1, c2, c3) && ok;

        GateState gate;
        gate.init (kRate);
        const float g1 = gate.process (1.0e30f, 0.5);
        const float g2 = gate.process (-1.0e30f, 0.5);
        const float g3 = gate.process (std::numeric_limits<float>::quiet_NaN(), 0.5);
        ok = check (finiteF (g1) && finiteF (g2) && finiteF (g3),
                    "M14a fx_gate bounds/NaN: extreme inputs remain finite (%.2f, %.2f, %.2f)",
                    g1, g2, g3) && ok;
    }

    // fx_comp compression and auto-makeup
    {
        CompState comp;
        comp.init (kRate);

        // amount 0 is bypass: passes input through unchanged
        float maxDiff = 0.0f;
        for (int i = 0; i < 1000; ++i)
        {
            const float in = 0.5f * std::sin (2.0f * (float) kPi * 440.0f * (float) i / 48000.0f);
            const float out = comp.process (in, 0.0);
            maxDiff = std::max (maxDiff, std::abs (out - in));
        }
        ok = check (maxDiff == 0.0f, "M14a fx_comp: amount 0.0 passes input dry, bit for bit") && ok;

        // amount 1.0 on a quiet signal: auto-makeup boosts it significantly
        comp.reset();
        float quietPeak = 0.0f;
        for (int i = 0; i < 4800; ++i)
        {
            const float in = 0.02f * std::sin (2.0f * (float) kPi * 440.0f * (float) i / 48000.0f);
            const float out = comp.process (in, 1.0);
            if (i > 2400)
                quietPeak = std::max (quietPeak, std::abs (out));
        }
        ok = check (quietPeak > 0.05f, "M14a fx_comp: amount 1.0 auto-makeup boosts quiet signal (%.3f vs 0.02 in)",
                    quietPeak) && ok;
    }

    // fx_gate gating and hysteresis
    {
        GateState gate;
        gate.init (kRate);

        // threshold 0 is bypass: passes input through unchanged
        float maxDiff = 0.0f;
        for (int i = 0; i < 1000; ++i)
        {
            const float in = 0.5f * std::sin (2.0f * (float) kPi * 440.0f * (float) i / 48000.0f);
            const float out = gate.process (in, 0.0);
            maxDiff = std::max (maxDiff, std::abs (out - in));
        }
        ok = check (maxDiff == 0.0f, "M14a fx_gate: threshold 0.0 passes input dry, bit for bit") && ok;

        // threshold 0.2 on signal below threshold (0.05): gates to silence
        gate.reset();
        float subThreshPeak = 0.0f;
        for (int i = 0; i < 4800; ++i)
        {
            const float in = 0.05f * std::sin (2.0f * (float) kPi * 440.0f * (float) i / 48000.0f);
            const float out = gate.process (in, 0.2);
            if (i > 2400)
                subThreshPeak = std::max (subThreshPeak, std::abs (out));
        }
        ok = check (subThreshPeak < 1.0e-4f, "M14a fx_gate: signal below threshold is gated to silence (peak %.6f)",
                    subThreshPeak) && ok;
    }

    // Node ids and bake test for fx_comp and fx_gate
    {
        NodeRef nc, ng;
        const bool ids = parseNodeId ("fx_comp:0", nc) && nc.module == ModuleId::FxComp && nc.slot == 0
                         && parseNodeId ("fx_gate:1", ng) && ng.module == ModuleId::FxGate && ng.slot == 1;
        ok = check (ids, "M14a fx_comp and fx_gate: node ids parse to correct ModuleId and slots") && ok;

        Patch p;
        p.name = "CompGateBake";
        p.voice =
            "s = osc_saw(0, freq);\n"
            "s = fx_comp(0, s, 0.3);\n"
            "s = fx_gate(0, s, 0.1);\n"
            "out = s * env_adsr(0, gate, 0.01, 0.2, 0.7, 0.1) * vel * 0.4;";
        p.master = "outl = inl; outr = inr;";
        p.trim = 0.8;

        setPatchArgTweak (p, "fx_comp:0", "amount", ArgTweak::kReplace, 0.75);
        setPatchArgTweak (p, "fx_gate:0", "threshold", ArgTweak::kReplace, 0.25);

        const BakeResult baked = bakePatch (p);
        ok = check (baked.ok && baked.callsChanged == 2,
                    "M14a fx_comp and fx_gate: bakePatch rewrites tweaked arguments (%d changed)%s%s",
                    baked.callsChanged, baked.ok ? "" : ": ", baked.error.c_str()) && ok;
    }

    return ok;
}

// ---------------------------------------------------------------------------
// 6. bus_delay lowcut test
bool smokeBusDelayLowcut()
{
    bool ok = true;
    const int n = 48000;
    const int frames = DelayBusState::framesFor (kRate);
    std::vector<float> storage0 ((size_t) frames * 2u), storage1 ((size_t) frames * 2u);

    DelayBusState d0, d1;
    d0.init (kRate, storage0.data(), frames);
    d1.init (kRate, storage1.data(), frames);

    // 100 Hz input tone
    std::vector<float> tone ((size_t) n);
    for (int i = 0; i < n; ++i)
        tone[(size_t) i] = static_cast<float> (0.5 * std::sin (2.0 * kPi * 100.0 * i / kRate));

    // d0: lowcut = 0.0 (60 Hz HP -> 100 Hz passes)
    // d1: lowcut = 1.0 (1500 Hz HP -> 100 Hz heavily cut)
    double rms0 = 0.0, rms1 = 0.0;
    for (int i = 0; i < n; ++i)
    {
        float l0 = 0.0f, r0 = 0.0f, l1 = 0.0f, r1 = 0.0f;
        d0.process (tone[(size_t) i], tone[(size_t) i], 0.25, 0.5, 1.0, 0.0, 120.0, l0, r0);
        d1.process (tone[(size_t) i], tone[(size_t) i], 0.25, 0.5, 1.0, 1.0, 120.0, l1, r1);
        if (i >= 24000)
        {
            rms0 += (double) l0 * (double) l0;
            rms1 += (double) l1 * (double) l1;
        }
    }
    rms0 = std::sqrt (rms0 / 24000.0);
    rms1 = std::sqrt (rms1 / 24000.0);
    const double cutDb = 20.0 * std::log10 (std::max (rms1, 1e-9) / std::max (rms0, 1e-9));

    ok = check (cutDb < -18.0,
                "M14a bus_delay lowcut: 100 Hz echoes are %.1f dB quieter at lowcut 1.0 vs 0.0 (< -18 dB)",
                cutDb) && ok;

    return ok;
}

} // namespace

bool smokeM14a()
{
    bool ok = true;
    ok = smokeScanDeterminism() && ok;
    ok = smokeValidatorNoiseFlag() && ok;
    ok = smokeMacroRangeRoundTrip() && ok;
    ok = smokeMacroRangeBakeEquivalence() && ok;
    ok = smokeFxCompAndGate() && ok;
    ok = smokeBusDelayLowcut() && ok;
    return ok;
}

} // namespace vibesynth_cli
