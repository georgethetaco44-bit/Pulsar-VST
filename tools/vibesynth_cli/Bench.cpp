// M4: vibesynth_cli --bench (SPEC §4.2.1, §10.3). Report-only, never
// gating: prints ns per voice-sample for every lexicon function and for
// every built-in preset (M5: resources/presets/*.pulsar), each the MEDIAN
// of 7 runs of 1 s at 48 kHz (single runs vary up to 2x on the Deck) and
// marked PASS/FAIL against the 300 ns voice budget (M6: a mono preset's
// voice against 1,200 ns; bus_* and preset masters against the 2 µs master
// budget).

#include "Bench.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "patch/Patch.h"
#include "patch/PatchEngine.h"

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
    { "bus_reverb", "out = osc_saw(0, freq);", "outl = bus_reverb(inl, inr, 0.5, 0.3); outr = bus_reverb_r();", true },
};

const int kNumBenchCases = static_cast<int> (sizeof (kBenchCases) / sizeof (kBenchCases[0]));

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kSamples = 48000; // 1 s
constexpr int kRuns = 7;
constexpr double kBenchFreq = 220.0;
constexpr double kVoiceBudgetNs = vibesynth::Patch::kPolyVoiceBudgetNs;  // SPEC §4.2.1
constexpr double kMasterBudgetNs = vibesynth::Patch::kMasterBudgetNs;   // ≈10 % of a core

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
                  int runs = kRuns, int samples = kSamples)
{
    volatile float sink = 0.0f;
    std::vector<double> times;
    vibesynth::Transport t;
    const double step = t.beatStep (kSampleRate);
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

std::vector<float> renderInput (vibesynth::EelVoice& v, double freq,
                                const double macros[vibesynth::MacroSmoother::kNumMacros],
                                int samples = kSamples)
{
    std::vector<float> out ((size_t) samples, 0.0f);
    vibesynth::Transport t;
    const double step = t.beatStep (kSampleRate);
    v.noteOn (freq, 57.0, 0.9, t);
    for (int i = 0; i < samples; ++i)
    {
        t.beat += step;
        out[(size_t) i] = v.renderSample (freq, 1.0, 57.0, 0.9, macros, t);
    }
    return out;
}

void printRow (const char* name, double ns, double base, double budget)
{
    std::printf ("%-24s %8.1f ns  %+8.1f  %6.0f  [%s]\n", name, ns, ns - base,
                 budget, ns <= budget ? "PASS" : "FAIL");
}

void fillMacros (double macros[vibesynth::MacroSmoother::kNumMacros], double v)
{
    for (int i = 0; i < vibesynth::MacroSmoother::kNumMacros; ++i)
        macros[i] = v;
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

PresetCost measurePresetCost (const vibesynth::Patch& patch, int runs, int samples)
{
    using namespace vibesynth;
    PresetCost cost;
    const PatchStrings ps = { patch.init.c_str(), patch.voice.c_str(), patch.master.c_str() };
    double macros[MacroSmoother::kNumMacros] = {};
    for (int m = 0; m < MacroSmoother::kNumMacros; ++m)
        macros[m] = patch.macroDefault (m);
    const auto v = std::make_unique<EelVoice> (ps, kSampleRate, 9u);
    const auto m = std::make_unique<EelMaster> (ps, kSampleRate);
    if (! v->isValid() || ! m->isValid())
        return cost;
    cost.voiceNs = timeVoice (*v, 55.0, 33.0, macros, runs, samples);
    const std::vector<float> in = renderInput (*v, 55.0, macros, samples);
    cost.masterNs = timeMaster (*m, in, macros, runs, samples);
    cost.ok = true;
    return cost;
}

int runBench()
{
    using namespace vibesynth;

    double macros[MacroSmoother::kNumMacros] = {};
    fillMacros (macros, 0.5);

    std::printf ("\n-- bench: ns per voice-sample, median of %d x 1 s @ 48 kHz --\n", kRuns);
    std::printf ("voice cases: budget %.0f ns per voice-sample incl. execute overhead;\n"
                 "bus_* cases: master VM only, budget %.0f ns per sample (SPEC 4.2.1).\n"
                 "'vs base' subtracts the matching baseline (voice: out = 0; master:\n"
                 "outl = inl; outr = inr;). Cases fed by osc_saw(1, ...) include it;\n"
                 "the (swept) filter cases also include an osc_sine LFO on the cutoff.\n\n",
                 kVoiceBudgetNs, kMasterBudgetNs);
    std::printf ("%-24s %11s  %8s  %6s\n", "case", "ns", "vs base", "budget");

    warmUp();

    double voiceBase = 0.0;
    double masterBase = 0.0;
    double sawNs = 0.0;
    double ladderNs = 0.0;
    for (int ci = 0; ci < kNumBenchCases; ++ci)
    {
        const BenchCase& bc = kBenchCases[ci];
        const PatchStrings ps = { "fb1 = 0; prev = 0;", bc.voice, bc.master };
        // Heap, not stack: EelVoice is ~87 KB (Windows' default stack is 1 MB).
        const auto vp = std::make_unique<EelVoice> (ps, kSampleRate, 7u);
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
            const std::vector<float> in = renderInput (v, kBenchFreq, macros);
            const double ns = timeMaster (m, in, macros);
            if (masterBase == 0.0)
                masterBase = ns; // the first master case is the master baseline
            printRow (bc.name, ns, masterBase, kMasterBudgetNs);
        }
        else
        {
            const double ns = timeVoice (v, kBenchFreq, 57.0, macros);
            if (ci == 0)
                voiceBase = ns;
            if (std::string (bc.name) == "osc_saw")
                sawNs = ns;
            if (std::string (bc.name) == "flt_ladder")
                ladderNs = ns;
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

        const auto pvPtr = std::make_unique<EelVoice> (ps, kSampleRate, 9u);
        const auto pmPtr = std::make_unique<EelMaster> (ps, kSampleRate);
        if (! pvPtr->isValid() || ! pmPtr->isValid())
        {
            std::printf ("%s COMPILE FAIL: %s %s\n", builtinPresetFile (pi),
                         pvPtr->compileError().c_str(), pmPtr->compileError().c_str());
            continue;
        }
        const double vNs = timeVoice (*pvPtr, 55.0, 33.0, presetMacros);
        const std::vector<float> in = renderInput (*pvPtr, 55.0, presetMacros);
        const double mNs = timeMaster (*pmPtr, in, presetMacros);
        const std::string vName = patch.name + " voice";
        const std::string mName = patch.name + " master";
        printRow (vName.c_str(), vNs, voiceBase, patch.voiceBudgetNs());
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
            const auto avPtr = std::make_unique<EelVoice> (alt, kSampleRate, 9u);
            if (avPtr->isValid())
                printRow (usesLadder ? "  variant with flt_lp" : "  variant with ladder",
                          timeVoice (*avPtr, 55.0, 33.0, presetMacros), voiceBase, patch.voiceBudgetNs());
        }
    }

    // Ladder's own cost (for the M7 decision on an own 1x ladder): Daisy
    // runs it 4x oversampled internally, so at a 2x voice rate it is paid
    // twice per host sample (8x total).
    const double ladderOwn = ladderNs - sawNs;
    std::printf ("\nflt_ladder own cost: %.1f ns per call (flt_ladder case minus osc_saw);\n"
                 "  at the M7 2x voice rate: %.1f ns per host sample\n",
                 ladderOwn, 2.0 * ladderOwn);

    // Budget arithmetic (SPEC 4.2.1): 8 voices, 48 kHz, 2x oversampling
    // from M7 on; one core = 1e9 ns per second.
    const double voice1x = voiceNs * 8.0 * kSampleRate / 1e7;
    const double voice2x = voice1x * 2.0;
    const double masterPct = masterNs * kSampleRate / 1e7;
    std::printf ("default preset, projected 8 voices @48 kHz: %.1f%% of a core at 1x, "
                 "%.1f%% at 2x (budget 25%%)\n", voice1x, voice2x);
    std::printf ("default preset, projected master @48 kHz: %.2f%% of a core (budget 10%%)\n",
                 masterPct);
    std::printf ("BENCH DONE (report-only; exit 0)\n");
    return 0;
}

} // namespace vibesynth_cli
