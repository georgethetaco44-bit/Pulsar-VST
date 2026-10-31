// M4: vibesynth_cli --bench (SPEC §4.2.1, §10.3). Report-only, never
// gating: prints ns per voice-sample for every lexicon function and for
// every built-in preset (M5: resources/presets/*.pulsar), each the MEDIAN
// of 7 runs of 48,000 voice samples (single runs vary up to 2x on the
// Deck) and marked PASS/FAIL against the 300 ns voice budget (M6: a mono
// preset's voice against 1,200 ns; bus_* and preset masters against the
// 2 µs master budget). M7: voice VMs run at the oversampled voice rate
// (2 x 48 kHz unless --os 1), so the voice budgets are per OVERSAMPLED
// sample; masters run at 48 kHz, fed through the half-band decimator.

#include "Bench.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "engine/Modulators.h"
#include "engine/NodeOverrides.h"
#include "engine/Oversampler.h"
#include "patch/Patch.h"
#include "patch/PatchEngine.h"
#include "patch/PatchTweaks.h"

namespace vibesynth_cli {

const BenchCase kBenchCases[] = {
    { "baseline (out = 0)", "out = 0;", "outl = inl; outr = inr;", false },
    { "osc_sine", "out = osc_sine(0, freq);", "outl = inl; outr = inr;", false },
    { "osc_saw", "out = osc_saw(0, freq);", "outl = inl; outr = inr;", false },
    { "osc_tri", "out = osc_tri(0, freq);", "outl = inl; outr = inr;", false },
    { "osc_square", "out = osc_square(0, freq, 0.3);", "outl = inl; outr = inr;", false },
    { "supersaw", "out = osc_supersaw(0, freq, 0.5, 0.6);", "outl = inl; outr = inr;", false },
    { "noise", "out = noise();", "outl = inl; outr = inr;", false },
    { "flt_lp", "out = flt_lp(0, osc_saw(1, freq), 0.5, 0.3);", "outl = inl; outr = inr;", false },
    { "flt_hp", "out = flt_hp(0, osc_saw(1, freq), 0.5, 0.3);", "outl = inl; outr = inr;", false },
    { "flt_bp", "out = flt_bp(0, osc_saw(1, freq), 0.5, 0.3);", "outl = inl; outr = inr;", false },
    { "flt_ladder", "out = flt_ladder(0, osc_saw(1, freq), 0.5, 0.5);", "outl = inl; outr = inr;", false },
    { "flt_lp (swept)", "out = flt_lp(0, osc_saw(1, freq), 0.4 + 0.3 * osc_sine(2, 5), 0.3);", "outl = inl; outr = inr;", false },
    { "flt_ladder (swept)", "out = flt_ladder(0, osc_saw(1, freq), 0.4 + 0.3 * osc_sine(2, 5), 0.5);", "outl = inl; outr = inr;", false },
    { "env_adsr", "out = env_adsr(0, gate, 0.1, 0.3, 0.6, 0.3);", "outl = inl; outr = inr;", false },
    { "env_follow", "out = env_follow(0, osc_saw(1, freq));", "outl = inl; outr = inr;", false },
    { "fx_fold", "out = fx_fold(osc_saw(0, freq), 0.5);", "outl = inl; outr = inr;", false },
    { "fx_sat", "out = fx_sat(osc_saw(0, freq), 0.5);", "outl = inl; outr = inr;", false },
    { "fx_crush", "out = fx_crush(0, osc_saw(1, freq), 0.5, 0.5);", "outl = inl; outr = inr;", false },
    { "fx_comb", "out = fx_comb(0, osc_saw(1, freq), 220.0, 0.5);", "outl = inl; outr = inr;", false },
    { "fx_ott", "out = fx_ott(0, osc_saw(1, freq) * 0.3, 0.8);", "outl = inl; outr = inr;", false },
    { "resonator", "out = phys_resonator(0, osc_saw(1, freq), 220.0, 0.5, 0.5, 0.5) * 0.25;", "outl = inl; outr = inr;", false },
    { "string", "out = phys_string(0, noise() * 0.2, 220.0, 0.5, 0.5);", "outl = inl; outr = inr;", false },
    { "lorenz", "out = chaos_lorenz(0, 0.5, 0.0);", "outl = inl; outr = inr;", false },
    { "lorenz (rate 1)", "out = chaos_lorenz(0, 1.0, 0.0);", "outl = inl; outr = inr;", false },
    { "duffing", "out = chaos_duffing(0, 2.0, 0.5);", "outl = inl; outr = inr;", false },
    { "safe_fb", "fb1 = util_safe_fb(0, osc_saw(1, freq) * 0.5 + fb1 * 0.3); out = fb1;", "outl = inl; outr = inr;", false },
    { "xover", "out = util_xover_lo(0, osc_saw(1, freq)) + util_xover_hi(0);", "outl = inl; outr = inr;", false },
    { "bytebeat", "out = util_bytebeat(1000.0 + freq * 10.0, 0.0) * 0.5;", "outl = inl; outr = inr;", false },
    { "master baseline", "out = osc_saw(0, freq);", "outl = inl; outr = inr;", true },
    { "bus_sat", "out = osc_saw(0, freq);", "outl = bus_sat(inl, 0.4); outr = bus_sat(inr, 0.4);", true },
    { "bus_mono_low", "out = osc_saw(0, freq);", "outl = bus_mono_low(inl, inr); outr = bus_mono_low_r();", true },
    { "bus_chorus", "out = osc_saw(0, freq);", "outl = bus_chorus(inl, inr, 0.4, 0.4); outr = bus_chorus_r();", true },
    { "fx_ott (master, 2 slots)", "out = osc_saw(0, freq);", "outl = fx_ott(0, inl, 0.8); outr = fx_ott(1, inr, 0.8);", true },
    { "bus_reverb", "out = osc_saw(0, freq);", "outl = bus_reverb(inl, inr, 0.5, 0.3); outr = bus_reverb_r();", true },
};

const int kNumBenchCases = static_cast<int> (sizeof (kBenchCases) / sizeof (kBenchCases[0]));

namespace {

constexpr double kSampleRate = 48000.0;  // host rate
constexpr int kSamples = 48000; // timed samples per run (voice or host)
constexpr int kRuns = 7;
constexpr double kBenchFreq = 220.0;
constexpr double kVoiceBudgetNs = vibesynth::Patch::kPolyVoiceBudgetNs;  // SPEC §4.2.1
constexpr double kMasterBudgetNs = vibesynth::Patch::kMasterBudgetNs;   // ≈10 % of a core
constexpr double kSvfTargetNs = 30.0;     // M7 own-filter targets, per call
constexpr double kLadderTargetNs = 60.0;

using Clock = std::chrono::steady_clock;

double nsPerSample (Clock::time_point t0, Clock::time_point t1, int samples)
{
    return std::chrono::duration<double, std::nano> (t1 - t0).count() / samples;
}

double median (std::vector<double> v)
{
    std::sort (v.begin(), v.end());
    return v[v.size() / 2];
}

// Median of `runs` x `samples` of voice rendering (after one untimed
// pass of `samples`), gate held, beat
// advancing at 120 bpm (so tempo-synced code takes its real paths).
double timeVoice (vibesynth::EelVoice& v, double freq, double note,
                  const double macros[vibesynth::MacroSmoother::kNumMacros],
                  double voiceRate, int runs = kRuns, int samples = kSamples)
{
    volatile float sink = 0.0f;
    std::vector<double> times;
    vibesynth::Transport t;
    const double step = t.beatStep (voiceRate);
    v.noteOn (freq, note, 0.9, t);
    // Run -1 is the untimed warm-up pass (SPEC §4.2.1: the clock ramps).
    for (int r = -1; r < runs; ++r)
    {
        float acc = 0.0f;
        const auto t0 = Clock::now();
        for (int i = 0; i < samples; ++i)
        {
            t.beat += step;
            acc += v.renderSample (freq, 1.0, note, 0.9, macros, t);
        }
        const auto t1 = Clock::now();
        sink = sink + acc;
        if (r >= 0)
            times.push_back (nsPerSample (t0, t1, samples));
    }
    return median (times);
}

// M13: median of `runs` x `samples` of voice rendering with 2 LFOs, 2 ADSR
// envelopes and 4 active modulation assignments (2 macros + 2 node knobs).
double timeVoiceWithMods (vibesynth::EelVoice& v, const vibesynth::Patch& basePatch,
                          double freq, double note,
                          const double macros[vibesynth::MacroSmoother::kNumMacros],
                          double voiceRate, int runs = kRuns, int samples = kSamples)
{
    using namespace vibesynth;
    Patch modPatch = basePatch;
    setPatchMacroMod (modPatch, ModSource::Lfo1, 1, 0.35);
    setPatchMacroMod (modPatch, ModSource::Env1, 2, 0.40);
    setPatchArgMod (modPatch, ModSource::Lfo2, "flt_lp:0", "cutoff", 0.30);
    setPatchArgMod (modPatch, ModSource::Env2, "flt_lp:0", "res", -0.25);

    auto table = std::make_unique<OverrideTable>();
    applyPatchTweaks (modPatch, *table);
    v.rack().overrides = table.get();
    v.rackContext().hooked = true;

    VoiceModPack pack;
    pack.prepare (voiceRate, 9);
    std::array<ModLfoSpec, kNumModLfos> lfoSpecs { table->getLfoSpec (0), table->getLfoSpec (1) };
    std::array<ModEnvSpec, kNumModEnvs> envSpecs { table->getEnvSpec (0), table->getEnvSpec (1) };
    uint8_t macroMask[MacroSmoother::kNumMacros] = {};
    double macroDepth[MacroSmoother::kNumMacros][kNumModSources] = {};
    for (int m = 0; m < MacroSmoother::kNumMacros; ++m)
    {
        macroMask[m] = table->macroModMask[m].load (std::memory_order_acquire);
        for (int src = 0; src < kNumModSources; ++src)
            macroDepth[m][src] = table->macroModDepth[m][src].load (std::memory_order_relaxed);
    }

    volatile float sink = 0.0f;
    std::vector<double> times;
    Transport t;
    const double step = t.beatStep (voiceRate);
    v.noteOn (freq, note, 0.9, t);
    pack.noteOn ((int) note);
    for (int r = -1; r < runs; ++r)
    {
        float acc = 0.0f;
        const auto t0 = Clock::now();
        for (int i = 0; i < samples; ++i)
        {
            t.beat += step;
            pack.stepAll (lfoSpecs, envSpecs, t.bpm, t.beat, 1.0);
            auto& vr = v.rack();
            for (int src = 0; src < kNumModSources; ++src)
                vr.modValues[src] = pack.values[(size_t) src];
            double voiceMacros[MacroSmoother::kNumMacros];
            for (int m = 0; m < MacroSmoother::kNumMacros; ++m)
            {
                double vm = macros[m];
                const uint8_t mMask = macroMask[m];
                if (mMask != 0u)
                {
                    double delta = 0.0;
                    for (int src = 0; src < kNumModSources; ++src)
                        if ((mMask & (1u << src)) != 0u)
                            delta += macroDepth[m][src] * pack.values[(size_t) src];
                    vm = std::clamp (vm + delta, 0.0, 1.0);
                }
                voiceMacros[m] = vm;
            }
            acc += v.renderSample (freq, 1.0, note, 0.9, voiceMacros, t);
        }
        const auto t1 = Clock::now();
        sink = sink + acc;
        if (r >= 0)
            times.push_back (nsPerSample (t0, t1, samples));
    }
    v.rack().overrides = nullptr;
    v.rackContext().hooked = false;
    return median (times);
}

// Median of `runs` x `samples` of master rendering (after one untimed
// pass), fed from a
// pre-rendered input (at least `samples` long).
double timeMaster (vibesynth::EelMaster& m, const std::vector<float>& in,
                   const double macros[vibesynth::MacroSmoother::kNumMacros],
                   int runs = kRuns, int samples = kSamples)
{
    volatile float sink = 0.0f;
    std::vector<double> times;
    vibesynth::Transport t;
    const double step = t.beatStep (kSampleRate);
    for (int r = -1; r < runs; ++r)   // -1: untimed warm-up pass
    {
        float acc = 0.0f;
        const auto t0 = Clock::now();
        for (int i = 0; i < samples; ++i)
        {
            float oL = 0.0f, oR = 0.0f;
            t.beat += step;
            m.renderSample (in[(size_t) i], in[(size_t) i], oL, oR, macros, t);
            acc += oL + oR;
        }
        const auto t1 = Clock::now();
        sink = sink + acc;
        if (r >= 0)
            times.push_back (nsPerSample (t0, t1, samples));
    }
    return median (times);
}

// `samples` host samples of master input: the voice at `os` x 48 kHz,
// through the half-band decimator when os = 2 (the plugin's path).
std::vector<float> renderInput (vibesynth::EelVoice& v, double freq,
                                const double macros[vibesynth::MacroSmoother::kNumMacros],
                                int os, int samples = kSamples)
{
    std::vector<float> out ((size_t) samples, 0.0f);
    vibesynth::Transport t;
    const double step = t.beatStep (kSampleRate * os);
    vibesynth::HalfbandDecimator decimator;
    v.noteOn (freq, 57.0, 0.9, t);
    for (int i = 0; i < samples; ++i)
    {
        float pair[2] = {};
        for (int k = 0; k < os; ++k)
        {
            t.beat += step;
            pair[k] = v.renderSample (freq, 1.0, 57.0, 0.9, macros, t);
        }
        out[(size_t) i] = os == 2 ? decimator.process (pair[0], pair[1]) : pair[0];
    }
    return out;
}

void fillMacros (double macros[vibesynth::MacroSmoother::kNumMacros], double v)
{
    for (int i = 0; i < vibesynth::MacroSmoother::kNumMacros; ++i)
        macros[i] = v;
}

// A lexicon call's own cost: the median over 9 rounds of (test - ref),
// each round timing `ref` then `test` back to back (the code differs only
// by the call under test), so clock drift and run-to-run noise cancel.
double ownCost (const char* ref, const char* test, double voiceRate)
{
    const vibesynth::PatchStrings pr = { "0;", ref, "outl = inl; outr = inr;" };
    const vibesynth::PatchStrings pt = { "0;", test, "outl = inl; outr = inr;" };
    const auto vr = std::make_unique<vibesynth::EelVoice> (pr, voiceRate, 7u);
    const auto vt = std::make_unique<vibesynth::EelVoice> (pt, voiceRate, 7u);
    if (! vr->isValid() || ! vt->isValid())
        return -1.0;
    double macros[vibesynth::MacroSmoother::kNumMacros] = {};
    fillMacros (macros, 0.5);
    std::vector<double> diffs;
    for (int r = 0; r < 9; ++r)
    {
        const double a = timeVoice (*vr, kBenchFreq, 57.0, macros, voiceRate, 1, kSamples / 2);
        const double b = timeVoice (*vt, kBenchFreq, 57.0, macros, voiceRate, 1, kSamples / 2);
        diffs.push_back (b - a);
    }
    return median (diffs);
}

// Median ns per host sample of the half-band decimator alone.
double timeDecimator()
{
    vibesynth::HalfbandDecimator d;
    std::vector<float> in ((size_t) kSamples * 2);
    for (size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float> (std::sin (0.01 * static_cast<double> (i)));
    volatile float sink = 0.0f;
    std::vector<double> times;
    for (int r = -1; r < kRuns; ++r)
    {
        float acc = 0.0f;
        const auto t0 = Clock::now();
        for (int i = 0; i < kSamples; ++i)
            acc += d.process (in[(size_t) (2 * i)], in[(size_t) (2 * i + 1)]);
        const auto t1 = Clock::now();
        sink = sink + acc;
        if (r >= 0)
            times.push_back (nsPerSample (t0, t1, kSamples));
    }
    return median (times);
}

void printRow (const char* name, double ns, double base, double budget)
{
    std::printf ("%-24s %8.1f ns  %+8.1f  %6.0f  [%s]\n", name, ns, ns - base,
                 budget, ns <= budget ? "PASS" : "FAIL");
}


// Replaces the first `from` call in `code` with `to` (filter variants).
std::string swapCall (const char* code, const std::string& from, const std::string& to)
{
    std::string s (code);
    const size_t at = s.find (from);
    if (at != std::string::npos)
        s.replace (at, from.size(), to);
    return s;
}

} // namespace

// Spins the CPU up to its sustained clock before anything is timed (the
// first timed case otherwise read up to 3x slow).
void warmUp()
{
    const vibesynth::PatchStrings ps = { "0;", "out = osc_saw(0, freq);", "outl = inl; outr = inr;" };
    const auto vp = std::make_unique<vibesynth::EelVoice> (ps, kSampleRate, 1u);
    vibesynth::EelVoice& v = *vp;
    double macros[vibesynth::MacroSmoother::kNumMacros] = {};
    fillMacros (macros, 0.5);
    volatile float sink = 0.0f;
    const auto until = Clock::now() + std::chrono::milliseconds (500);
    while (Clock::now() < until)
        for (int i = 0; i < 4800; ++i)
            sink = sink + v.renderSample (kBenchFreq, 1.0, 57.0, 0.9, macros);
}

PresetCost measurePresetCost (const vibesynth::Patch& patch, int runs, int samples,
                              int oversampling)
{
    using namespace vibesynth;
    PresetCost cost;
    const PatchStrings ps = { patch.init.c_str(), patch.voice.c_str(), patch.master.c_str() };
    double macros[MacroSmoother::kNumMacros] = {};
    for (int m = 0; m < MacroSmoother::kNumMacros; ++m)
        macros[m] = patch.macroDefault (m);
    const double voiceRate = kSampleRate * oversampling;
    const auto v = std::make_unique<EelVoice> (ps, voiceRate, 9u);
    const auto m = std::make_unique<EelMaster> (ps, kSampleRate);
    if (! v->isValid() || ! m->isValid())
        return cost;
    cost.voiceNs = timeVoice (*v, 55.0, 33.0, macros, voiceRate, runs, samples);
    const std::vector<float> in = renderInput (*v, 55.0, macros, oversampling, samples);
    cost.masterNs = timeMaster (*m, in, macros, runs, samples);
    cost.ok = true;
    return cost;
}

int runBench (int oversampling)
{
    using namespace vibesynth;

    double macros[MacroSmoother::kNumMacros] = {};
    fillMacros (macros, 0.5);
    const int os = oversampling == 1 ? 1 : 2;
    const double voiceRate = kSampleRate * os;

    std::printf ("\n-- bench: ns per voice-sample, median of %d x %d samples; voices @ %.0f kHz "
                 "(%dx), master @ 48 kHz --\n", kRuns, kSamples, voiceRate / 1000.0, os);
    std::printf ("voice cases: budget %.0f ns per (oversampled) voice-sample incl. execute overhead;\n"
                 "bus_* cases: master VM only, budget %.0f ns per host sample (SPEC 4.2.1).\n"
                 "'vs base' subtracts the matching baseline (voice: out = 0; master:\n"
                 "outl = inl; outr = inr;). Cases fed by osc_saw(1, ...) include it;\n"
                 "the (swept) filter cases also include an osc_sine LFO on the cutoff.\n\n",
                 kVoiceBudgetNs, kMasterBudgetNs);
    std::printf ("%-24s %11s  %8s  %6s\n", "case", "ns", "vs base", "budget");

    warmUp();

    double voiceBase = 0.0;
    double masterBase = 0.0;
    for (int ci = 0; ci < kNumBenchCases; ++ci)
    {
        const BenchCase& bc = kBenchCases[ci];
        const PatchStrings ps = { "fb1 = 0; prev = 0;", bc.voice, bc.master };
        // Heap, not stack: EelVoice is ~175 KB since M7 (Windows' default stack is 1 MB).
        const auto vp = std::make_unique<EelVoice> (ps, voiceRate, 7u);
        const auto mp = std::make_unique<EelMaster> (ps, kSampleRate);
        EelVoice& v = *vp;
        EelMaster& m = *mp;
        if (! v.isValid() || ! m.isValid())
        {
            std::printf ("%-24s COMPILE FAIL: %s %s\n", bc.name,
                         v.compileError().c_str(), m.compileError().c_str());
            continue;
        }
        if (bc.isMaster)
        {
            const std::vector<float> in = renderInput (v, kBenchFreq, macros, os);
            const double ns = timeMaster (m, in, macros);
            if (masterBase == 0.0)
                masterBase = ns; // the first master case is the master baseline
            printRow (bc.name, ns, masterBase, kMasterBudgetNs);
        }
        else
        {
            const double ns = timeVoice (v, kBenchFreq, 57.0, macros, voiceRate);
            if (ci == 0)
                voiceBase = ns;
            printRow (bc.name, ns, voiceBase, kVoiceBudgetNs);
        }
    }

    // Every built-in preset (55 Hz, gate held, macros at the patch
    // defaults). Reese also gets its other-filter variant
    // (flt_lp <-> flt_ladder) for the M7 ladder decision.
    std::printf ("\n-- built-in presets (55 Hz, gate held, macro defaults; voice budget\n"
                 "   %.0f ns poly, %.0f ns mono) --\n", Patch::kPolyVoiceBudgetNs, Patch::kMonoVoiceBudgetNs);
    std::printf ("%-24s %11s  %8s  %6s\n", "part", "ns", "vs base", "budget");

    double voiceNs = 0.0;  // the default preset (index 0), for the projection
    double masterNs = 0.0;
    for (int pi = 0; pi < numBuiltinPresets(); ++pi)
    {
        const ParseResult pr = parsePatch (builtinPresetText (pi));
        if (! pr.ok())
        {
            std::printf ("%s PARSE FAIL: %s\n", builtinPresetFile (pi), pr.error.c_str());
            continue;
        }
        const Patch& patch = pr.patch;
        const PatchStrings ps = { patch.init.c_str(), patch.voice.c_str(), patch.master.c_str() };
        double presetMacros[MacroSmoother::kNumMacros] = {};
        for (int m = 0; m < MacroSmoother::kNumMacros; ++m)
            presetMacros[m] = patch.macroDefault (m);

        const auto pvPtr = std::make_unique<EelVoice> (ps, voiceRate, 9u);
        const auto pmPtr = std::make_unique<EelMaster> (ps, kSampleRate);
        if (! pvPtr->isValid() || ! pmPtr->isValid())
        {
            std::printf ("%s COMPILE FAIL: %s %s\n", builtinPresetFile (pi),
                         pvPtr->compileError().c_str(), pmPtr->compileError().c_str());
            continue;
        }
        const double vNs = timeVoice (*pvPtr, 55.0, 33.0, presetMacros, voiceRate);
        const std::vector<float> in = renderInput (*pvPtr, 55.0, presetMacros, os);
        const double mNs = timeMaster (*pmPtr, in, presetMacros);
        const std::string vName = patch.name + " voice";
        const std::string mName = patch.name + " master";
        printRow (vName.c_str(), vNs, voiceBase, patch.voiceBudgetNs());
        if (pi == 0)
        {
            const double modNs = timeVoiceWithMods (*pvPtr, patch, 55.0, 33.0, presetMacros, voiceRate);
            printRow ("Reese voice + 2 LFOs + 2 envs, 4 assignments", modNs, voiceBase, patch.voiceBudgetNs());
            std::printf ("  mod overhead vs plain Reese: %+.1f ns per voice-sample\n", modNs - vNs);
        }
        printRow (mName.c_str(), mNs, masterBase, kMasterBudgetNs);
        if (pi == 0)
        {
            voiceNs = vNs;
            masterNs = mNs;
            const bool usesLadder = patch.voice.find ("flt_ladder") != std::string::npos;
            const std::string altVoice = usesLadder
                ? swapCall (patch.voice.c_str(), "flt_ladder", "flt_lp")
                : swapCall (patch.voice.c_str(), "flt_lp", "flt_ladder");
            const PatchStrings alt = { patch.init.c_str(), altVoice.c_str(), patch.master.c_str() };
            const auto avPtr = std::make_unique<EelVoice> (alt, voiceRate, 9u);
            if (avPtr->isValid())
                printRow (usesLadder ? "  variant with flt_lp" : "  variant with ladder",
                          timeVoice (*avPtr, 55.0, 33.0, presetMacros, voiceRate), voiceBase,
                          patch.voiceBudgetNs());
        }
    }

    // M7 own filters, per call (interleaved pairs, see ownCost).
    struct OwnCase { const char* name; const char* ref; const char* test; double target; };
    const OwnCase own[] = {
        { "flt_lp (SVF)", "out = osc_saw(1, freq);",
          "out = flt_lp(0, osc_saw(1, freq), 0.5, 0.3);", kSvfTargetNs },
        { "flt_lp (SVF, swept)", "c = 0.4 + 0.3 * osc_sine(2, 5); out = osc_saw(1, freq);",
          "c = 0.4 + 0.3 * osc_sine(2, 5); out = flt_lp(0, osc_saw(1, freq), c, 0.3);", kSvfTargetNs },
        { "flt_ladder", "out = osc_saw(1, freq);",
          "out = flt_ladder(0, osc_saw(1, freq), 0.5, 0.5);", kLadderTargetNs },
        { "flt_ladder (swept)", "c = 0.4 + 0.3 * osc_sine(2, 5); out = osc_saw(1, freq);",
          "c = 0.4 + 0.3 * osc_sine(2, 5); out = flt_ladder(0, osc_saw(1, freq), c, 0.5);", kLadderTargetNs },
    };
    std::printf ("\n-- own filters per call at %.0f kHz (median of 9 interleaved with/without pairs) --\n",
                 voiceRate / 1000.0);
    for (const OwnCase& oc : own)
    {
        const double ns = ownCost (oc.ref, oc.test, voiceRate);
        std::printf ("%-24s %8.1f ns per call (target <= %.0f)  [%s]\n", oc.name, ns, oc.target,
                     ns <= oc.target ? "PASS" : "FAIL");
    }

    const double decimatorNs = os == 2 ? timeDecimator() : 0.0;
    std::printf ("half-band decimator: %.1f ns per host sample (one for all voices)\n", decimatorNs);

    // Budget arithmetic (SPEC 4.2.1): 8 voices at the voice rate, plus the
    // decimator and the master at 48 kHz; one core = 1e9 ns per second.
    const double voicePct = voiceNs * 8.0 * voiceRate / 1e7;
    const double decimatorPct = decimatorNs * kSampleRate / 1e7;
    const double masterPct = masterNs * kSampleRate / 1e7;
    std::printf ("default preset, projected 8 voices @ %.0f kHz (%dx): %.1f%% of a core "
                 "(budget 25%%), + decimator %.2f%%\n", voiceRate / 1000.0, os, voicePct, decimatorPct);
    std::printf ("default preset, projected master @48 kHz: %.2f%% of a core (budget 10%%)\n",
                 masterPct);
    std::printf ("BENCH DONE (report-only; exit 0)\n");
    return 0;
}

} // namespace vibesynth_cli
