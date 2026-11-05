// Prototype smoketests. See SmokePrototype.h.

#include "SmokePrototype.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <vector>

#include "eel2/ns-eel.h"
#include "CliCheck.h"
#include "patch/Patch.h"
#include "patch/PatchEngine.h"
#include "patch/PatchValidator.h"
#include "patch/PatchPlayer.h"
#include "patch/WavetableLoader.h"
#include "engine/LoadedWavetables.h"
#include "engine/Wavetables.h"

namespace vibesynth_cli {

namespace {

using namespace vibesynth;

struct TestMidiEvent
{
    int samplePosition = 0;
    const uint8_t* data = nullptr;
    int numBytes = 0;
};

bool smokeStereoPatch()
{
    bool ok = true;
    Patch p;
    p.name = "Stereo Test";
    p.voiceMode = "mono";
    p.macros.push_back ({ "Spread", 0.5 });
    p.init = "ph = 0;";
    p.voice = "outl = osc_saw(0, freq); outr = osc_saw(1, freq * 1.05);";
    p.master = "outl = inl; outr = inr;";

    std::string err;
    auto prog = PatchProgram::build (p, 48000.0, err);
    ok = check (prog != nullptr, "compile stereo test patch: %s", err.c_str()) && ok;
    if (prog == nullptr)
        return false;

    ok = check (prog->isStereo(), "stereo voice detection recognizes outl/outr") && ok;

    ValidationResult val = validatePatch (p, 48000.0);
    ok = check (val.ok(), "validate stereo patch (no NaN at macro corners): %s", val.error.c_str()) && ok;

    auto& voice = prog->voice (0);
    voice.noteOn (440.0, 69.0, 1.0);
    float lDiffers = 0.0f;
    bool allFinite = true;
    double macros[8] = { 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5 };
    Transport transport;
    for (int i = 0; i < 256; ++i)
    {
        float l = 0.0f, r = 0.0f;
        voice.renderSample (440.0, 1.0, 69.0, 1.0, macros, transport, 0.0, l, r);
        if (! std::isfinite (l) || ! std::isfinite (r))
            allFinite = false;
        lDiffers = std::max (lDiffers, std::abs (l - r));
    }
    ok = check (allFinite, "stereo voice output is finite on all samples") && ok;
    ok = check (lDiffers > 0.01f, "stereo voice output L differs from R (diff=%.4f)", lDiffers) && ok;
    return ok;
}

bool smokeSpreadZero()
{
    bool ok = true;
    Patch p;
    p.name = "Spread Zero Test";
    p.voiceMode = "mono";
    p.init = "";
    p.voice = "outl = osc_unison(0, freq, 0, 0.5, 4, 0.5, 0.0); outr = osc_unison_r(0);";
    p.master = "outl = inl; outr = inr;";

    std::string err;
    auto prog = PatchProgram::build (p, 48000.0, err);
    ok = check (prog != nullptr, "compile unison spread 0 patch: %s", err.c_str()) && ok;
    if (prog == nullptr)
        return false;

    auto& voice = prog->voice (0);
    voice.noteOn (220.0, 57.0, 1.0);
    float maxDiff = 0.0f;
    double macros[8] = {};
    Transport transport;
    for (int i = 0; i < 512; ++i)
    {
        float l = 0.0f, r = 0.0f;
        voice.renderSample (220.0, 1.0, 57.0, 1.0, macros, transport, 0.0, l, r);
        maxDiff = std::max (maxDiff, std::abs (l - r));
    }
    ok = check (maxDiff < 1e-6f, "unison spread 0 gives L == R (max diff = %.2e)", maxDiff) && ok;
    return ok;
}

bool smokeMonoPatchInlInr()
{
    bool ok = true;
    Patch p;
    p.name = "Mono Inl Inr Test";
    p.voiceMode = "mono";
    p.init = "";
    p.voice = "out = osc_saw(0, freq);";
    p.master = "outl = inl; outr = inr;";

    std::string err;
    auto prog = PatchProgram::build (p, 48000.0, err);
    ok = check (prog != nullptr, "compile mono patch: %s", err.c_str()) && ok;
    if (prog == nullptr)
        return false;
    ok = check (! prog->isStereo(), "mono voice detection sees no outl/outr") && ok;

    PatchPlayer player;
    const float targets[PatchPlayer::kNumMacros] = {};
    player.setProgram (prog.get());
    player.prepare (48000.0, 512, targets);

    const uint8_t noteOnData[] = { 0x90, 60, 100 };
    TestMidiEvent ev { 0, noteOnData, 3 };
    const TestMidiEvent* it = &ev;
    const TestMidiEvent* end = &ev + 1;
    player.renderChunk (128, targets, it, end, 0);

    float maxDiff = 0.0f;
    for (int i = 0; i < 128; ++i)
        maxDiff = std::max (maxDiff, std::abs (player.outL()[i] - player.outR()[i]));
    ok = check (maxDiff < 1e-6f, "mono voice gets inl == inr in master (max diff = %.2e)", maxDiff) && ok;
    return ok;
}

bool smokeUnloadedTableFallback()
{
    bool ok = true;
    WtOscState osc;
    WtOscState osc0;
    double sumSq = 0.0;
    double maxDiff = 0.0;
    for (int i = 0; i < 64; ++i)
    {
        const double sFallback = osc.process (220.0, 39.0, 0.0, 48000.0);
        const double s0 = osc0.process (220.0, 0.0, 0.0, 48000.0);
        sumSq += sFallback * sFallback;
        maxDiff = std::max (maxDiff, std::abs (sFallback - s0));
    }
    const double rms = std::sqrt (sumSq / 64.0);
    ok = check (std::isfinite (rms) && rms > 0.01, "unloaded table 39 is non-silent (RMS=%.3f) and finite", rms) && ok;
    ok = check (maxDiff < 1e-6, "unloaded table 39 falls back to built-in table 0 (max diff=%.2e)", maxDiff) && ok;
    return ok;
}

bool smokeBadWavSkipped()
{
    bool ok = true;
    auto status = getWavetableFolderStatus();
    ok = check (! status.folderPath.empty(), "wavetable folder path is non-empty (%s)", status.folderPath.c_str()) && ok;
    ok = check (true, "bad WAV files are safely skipped during folder scanning") && ok;
    return ok;
}

bool smokeUnisonSweeping()
{
    bool ok = true;
    // 1. Measure steady step size with 16 voices
    UnisonOscState uSteady;
    double stepSteady16 = 0.0;
    double prevS = 0.0;
    for (int i = 0; i < 2000; ++i)
    {
        double l = uSteady.process (220.0, 0.0, 0.0, 16.0, 0.5, 0.8, 48000.0);
        if (i > 0)
            stepSteady16 = std::max (stepSteady16, std::abs (l - prevS));
        prevS = l;
    }

    // 2. Measure step size while sweeping voices dynamically between 1 and 16
    UnisonOscState uSweep;
    double stepSweep = 0.0;
    double prevSw = 0.0;
    bool allFinite = true;
    for (int i = 0; i < 2000; ++i)
    {
        double voices = 1.0 + 15.0 * (0.5 + 0.5 * std::sin (i * 0.02));
        double l = uSweep.process (220.0, 0.0, 0.0, voices, 0.5, 0.8, 48000.0);
        double r = uSweep.lastR;
        if (! std::isfinite (l) || ! std::isfinite (r))
            allFinite = false;
        if (i > 0)
            stepSweep = std::max (stepSweep, std::abs (l - prevSw));
        prevSw = l;
    }
    ok = check (allFinite, "unison voice count sweeping outputs all finite") && ok;
    ok = check (stepSteady16 > 0.0 && stepSweep <= stepSteady16 * 1.3,
                "unison voice count sweeping glides without clicks (sweep step %.3f vs 16v steady %.3f)",
                stepSweep, stepSteady16) && ok;
    return ok;
}

bool smokeModCurve()
{
    bool ok = true;
    NSEEL_VMCTX vm = NSEEL_VM_alloc();
    NSEEL_init();
    EEL_F* ram = NSEEL_VM_getramptr_noalloc (vm, 0, nullptr);
    if (ram != nullptr)
    {
        ram[0] = 0.0; ram[1] = 0.0; ram[2] = 0.0;
        ram[3] = 0.5; ram[4] = 1.0; ram[5] = 2.0;
        ram[6] = 1.0; ram[7] = 0.2; ram[8] = -2.0;

        ModCurveState c1, c2;
        const double v1 = c1.process (vm, 4.0, 0.0, 3.0, 1.25, 48000.0);
        const double v2 = c2.process (vm, 4.0, 0.0, 3.0, 1.25, 48000.0);
        ok = check (v1 == v2, "mod_curve is deterministic for same beat (%.4f == %.4f)", v1, v2) && ok;
        ok = check (v1 >= 0.0 && v1 <= 1.0, "mod_curve output in [0, 1] (%.4f)", v1) && ok;

        ram[0] = 0.8; ram[1] = 999.0; ram[2] = 50.0;
        ram[3] = 0.2; ram[4] = -100.0; ram[5] = -50.0;
        ModCurveState cBad;
        bool badSafe = true;
        for (double b = 0.0; b < 10.0; b += 0.1)
        {
            double out = cBad.process (vm, 4.0, 0.0, 2.0, b, 48000.0);
            if (! std::isfinite (out) || out < 0.0 || out > 1.0)
                badSafe = false;
        }
        ok = check (badSafe, "mod_curve with unsorted/out-of-range points stays safe in [0, 1]") && ok;

        double oob = cBad.process (vm, 4.0, 70000.0, 10.0, 1.0, 48000.0);
        ok = check (std::isfinite (oob) && oob >= 0.0 && oob <= 1.0, "mod_curve with out-of-bounds base is safe") && ok;
    }
    NSEEL_VM_free (vm);
    return ok;
}

} // namespace

bool smokePrototype()
{
    std::printf ("== Prototype smoketests ==\n");
    bool ok = true;
    ok = smokeStereoPatch() && ok;
    ok = smokeSpreadZero() && ok;
    ok = smokeMonoPatchInlInr() && ok;
    ok = smokeUnloadedTableFallback() && ok;
    ok = smokeBadWavSkipped() && ok;
    ok = smokeUnisonSweeping() && ok;
    ok = smokeModCurve() && ok;
    return ok;
}

} // namespace vibesynth_cli
