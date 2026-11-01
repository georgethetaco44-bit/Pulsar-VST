// Headless test tool (SPEC §10.3). Shares the engine code with the plugin
// (links vibesynth_engine and compiles the same Patch.cpp; M5 made it a
// juce_core console app for juce::JSON), so CI tests exactly the code the
// plugin runs. Modes:
//   --smoketest  engine + safety gate in CI, exit 1 on any failure: the M3
//                sine checks (440 Hz zero crossings, an isolated 220 Hz
//                second voice, JIT mode), trig semantics, the steal fade,
//                the max error of the tanh/sin approximations, chaos/crush
//                sanity, every bench case compiling and rendering finite,
//                the default preset (Reese) held 2 s then released, and M5:
//                the 1024 loop cap (loop and while), the runtime-guard VM
//                reset, the output stage, and every tests/patches/broken_*
//                file rejected with its expected reason. M6: the
//                loop-nesting check (cases + tests/patches/ok_nested), a
//                worker thread validating broken_slow stopping promptly,
//                the master's own VoiceRack, bpm/beat, and the selftest's
//                trim tolerance and CPU budgets. M8 (SmokeM8.cpp, offline):
//                api_reference.md against the registered lexicon, the
//                lexicon lint, the response parser, the few-shot examples
//                through the validator, the prompt builder, the client's
//                error mapping and cancel, and self-heal on canned replies.
//   --selftest   the preset gate in CI (M6, exit 1 on any failure): every
//                built-in parses, round-trips, passes the offline test with
//                its stored trim within 1 dB of the computed one, builds a
//                PatchProgram, and has labelled macros and a prompt. Its CPU
//                cost (voice 300 ns poly / 1,200 ns mono, master 2 µs) is
//                reported; only --selftest --strict-cpu (the Deck, never CI)
//                fails a preset over budget (SPEC §4.2.1).
//   --bench      report-only (always exit 0); see Bench.cpp.
//   --validate   parse + offline-test one .pulsar file (exit 1 if rejected).
//   --render     play the test phrase through PatchPlayer into a WAV; see
//                Render.cpp.
//   --batch      (M8) generate a patch for every prompt in a file with the
//                real service and report pass rates; see Batch.cpp.
//   --change, --mutate, --change-batch  (M11) "Change this node" and
//                Mutate with the real service; see Edit.cpp.
//   --os 1|2     (M7, any mode but the tests) the voice oversampling for
//                --bench, --validate and --render; default 2 like the
//                plugin. 1 exists for A/B aliasing comparisons only.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include <string>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include <juce_core/juce_core.h>

#include "Batch.h"
#include "Bench.h"
#include "Edit.h"
#include "CliCheck.h"
#include "PulsarTestPatches.h"
#include "Render.h"
#include "SmokeM7.h"
#include "SmokeM8.h"
#include "SmokeM9.h"
#include "SmokeM10.h"
#include "SmokeM11.h"
#include "SmokeM12.h"
#include "SmokeM12b.h"
#include "SmokeM13.h"
#include "SmokeMerge.h"
#include "engine/OutputStage.h"
#include "patch/LexiconLint.h"
#include "patch/LoopNesting.h"
#include "patch/Patch.h"
#include "patch/PatchEngine.h"
#include "patch/PatchPlayer.h"
#include "patch/PatchValidator.h"

namespace {

using vibesynth::EelMaster;
using vibesynth::EelVoice;
using vibesynth::MacroSmoother;
using vibesynth::PatchStrings;

constexpr double kSampleRate = 48000.0;
constexpr int kOneSecond = 48000;
constexpr double kTestFreq = 440.0;
constexpr double kIsoFreq = 220.0; // second VM: proves per-voice isolation
constexpr const char* kPassMaster = "outl = inl; outr = inr;";

// True when this x86_64 process runs translated by Rosetta (macOS CI runs
// the Universal binary's x86_64 slice that way).
bool runningUnderRosetta()
{
#ifdef __APPLE__
    int translated = 0;
    size_t size = sizeof (translated);
    if (sysctlbyname ("sysctl.proc_translated", &translated, &size, nullptr, 0) == 0)
        return translated == 1;
#endif
    return false;
}

// Validator CPU scale (vibesynth::setValidatorCpuScale): 1 natively, 10
// under Rosetta. Set once in main(); the smoketest, selftest, batch,
// render and validate all read it through vibesynth::validatorCpuScale().
constexpr double kRosettaCpuScale = 10.0;

// --os (M7): voice oversampling for --bench, --validate and --render.
int g_oversampling = vibesynth::PatchProgram::kDefaultOversampling;

using vibesynth_cli::check;

bool isFiniteFloat (float v)
{
    uint32_t b = 0;
    std::memcpy (&b, &v, sizeof (v));
    return ((b >> 23) & 0xFFu) != 0xFFu;
}

bool allFinite (const std::vector<float>& x, size_t begin = 0)
{
    for (size_t i = begin; i < x.size(); ++i)
        if (! isFiniteFloat (x[i]))
            return false;
    return true;
}

double peakOf (const std::vector<float>& x, size_t begin, size_t end)
{
    double p = 0.0;
    for (size_t i = begin; i < end && i < x.size(); ++i)
    {
        const double a = std::fabs (static_cast<double> (x[i]));
        if (a > p)
            p = a;
    }
    return p;
}

double rmsOf (const std::vector<float>& x, size_t begin, size_t end)
{
    double sum = 0.0;
    size_t n = 0;
    for (size_t i = begin; i < end && i < x.size(); ++i, ++n)
        sum += static_cast<double> (x[i]) * x[i];
    return n > 0 ? std::sqrt (sum / static_cast<double> (n)) : 0.0;
}

int positiveZeroCrossings (const std::vector<float>& x, size_t begin = 0)
{
    int n = 0;
    for (size_t i = begin + 1; i < x.size(); ++i)
        if (x[i - 1] <= 0.0f && x[i] > 0.0f)
            ++n;
    return n;
}

void fillMacros (double macros[MacroSmoother::kNumMacros], double v)
{
    for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
        macros[i] = v;
}

// A voice-only program with a no-op init (EelVoice compiles in its ctor).
std::unique_ptr<EelVoice> makeVoice (const char* voiceCode, uint32_t seed)
{
    const PatchStrings ps = { "0;", voiceCode, kPassMaster };
    return std::make_unique<EelVoice> (ps, kSampleRate, seed);
}

bool compiled (const EelVoice& v, const char* what)
{
    if (v.isValid())
        return true;
    return check (false, "%s compiles: %s", what, v.compileError().c_str());
}

// Renders n samples of a held note at freq (no note-on is sent).
std::vector<float> renderHeld (EelVoice& v, double freq, int n)
{
    double macros[MacroSmoother::kNumMacros] = {};
    fillMacros (macros, 0.5);
    std::vector<float> out ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
        out[(size_t) i] = v.renderSample (freq, 1.0, 69.0, 0.9, macros);
    return out;
}

// ---------------------------------------------------------------------------
// M3 sine checks through the rack-backed lexicon osc_sine.

bool smokeSine()
{
    const char* program = "out = osc_sine(0, freq);";
    auto voice = makeVoice (program, 1u);
    auto iso = makeVoice (program, 2u);
    bool ok = compiled (*voice, "sine voice") && compiled (*iso, "isolation voice");
    if (ok)
    {
        check (true, "compile: \"%s\" (two independent VMs)", program);
        std::vector<float> a ((size_t) kOneSecond, 0.0f);
        std::vector<float> b ((size_t) kOneSecond, 0.0f);
        double macros[MacroSmoother::kNumMacros] = {};
        fillMacros (macros, 0.5);
        // Interleaved, as the plugin renders voices.
        for (int i = 0; i < kOneSecond; ++i)
        {
            a[(size_t) i] = voice->renderSample (kTestFreq, 1.0, 69.0, 0.9, macros);
            b[(size_t) i] = iso->renderSample (kIsoFreq, 1.0, 57.0, 0.9, macros);
        }
        ok = check (allFinite (a) && allFinite (b), "sine outputs finite") && ok;
        const double rms = rmsOf (a, 0, a.size());
        ok = check (rms >= 0.636 && rms <= 0.778,
                    "RMS %.5f (full-scale sine is 0.70711, want +/-10%%)", rms) && ok;
        const int za = positiveZeroCrossings (a);
        const int zb = positiveZeroCrossings (b);
        ok = check (za >= 431 && za <= 449,
                    "440 Hz tone: %d zero crossings in 1 s (want 440 +/-2%%)", za) && ok;
        ok = check (zb >= 215 && zb <= 225,
                    "per-voice isolation: 220 Hz voice has %d crossings (want 220 +/-2%%)",
                    zb) && ok;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// SPEC §4.3: trig is 1 on the note-on sample only. The program accumulates
// trig; out = 0.1 × accumulator, so after k note-ons out must be 0.1·k from
// the note-on sample onward and never change in between.

bool smokeTrig()
{
    auto v = makeVoice ("acc += trig; out = acc * 0.1;", 3u);
    if (! compiled (*v, "trig program"))
        return false;
    double macros[MacroSmoother::kNumMacros] = {};
    fillMacros (macros, 0.5);
    bool ok = true;
    for (int i = 0; i < 16; ++i)
        if (v->renderSample (110.0, 0.0, 45.0, 0.9, macros) != 0.0f)
            ok = false;
    ok = check (ok, "trig is 0 before any note-on") && ok;

    for (int k = 1; k <= 3; ++k)
    {
        v->noteOn (110.0, 45.0, 0.9);
        const float want = 0.1f * static_cast<float> (k);
        const float first = v->renderSample (110.0, 1.0, 45.0, 0.9, macros);
        bool steady = true;
        for (int i = 0; i < 256; ++i)
            if (std::fabs (v->renderSample (110.0, 1.0, 45.0, 0.9, macros) - want) > 1e-6f)
                steady = false;
        ok = check (std::fabs (first - want) < 1e-6f && steady,
                    "note-on %d: trig seen on the note-on sample (acc %.1f), "
                    "then 0 for 256 samples", k, first * 10.0f) && ok;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// VoiceLifetime steal fade (2 ms linear, then track() frees the voice).

bool smokeStealFade()
{
    vibesynth::VoiceLifetime lt;
    lt.noteOn();
    bool ok = check (! lt.track (0.0f, kSampleRate), "held voice is never freed");
    lt.startStealFade (kSampleRate);
    const int n = lt.fadeLength;
    float prev = 1.0f;
    bool monotonic = true;
    int samples = 0;
    while (! lt.track (0.5f, kSampleRate) && samples < 10 * n)
    {
        const float g = lt.fadeGain();
        if (g > prev || g < 0.0f)
            monotonic = false;
        prev = g;
        ++samples;
    }
    ok = check (n == 96 && samples == n && monotonic && prev == 0.0f,
                "steal fade: %d-sample linear ramp to 0 (want 96 = 2 ms), "
                "voice freed after %d samples", n, samples) && ok;
    return ok;
}

// ---------------------------------------------------------------------------
// SPEC §4.2.1 cheap approximations: max error against std:: on a dense grid.
// fastTanh feeds fx_sat, bus_sat and util_safe_fb; fastSin feeds fx_fold.

bool smokeApproximations()
{
    double tanhErr = 0.0, tanhAt = 0.0;
    constexpr int kSteps = 2000000;
    for (int i = 0; i <= kSteps; ++i)
    {
        const double x = -10.0 + 20.0 * i / kSteps;
        const double e = std::fabs (vibesynth::fastTanh (x) - std::tanh (x));
        if (e > tanhErr) { tanhErr = e; tanhAt = x; }
    }
    double sinErr = 0.0, sinAt = 0.0;
    for (int i = 0; i <= kSteps; ++i)
    {
        const double x = -1000.0 + 2000.0 * i / kSteps;
        const double e = std::fabs (vibesynth::fastSin (x) - std::sin (x));
        if (e > sinErr) { sinErr = e; sinAt = x; }
    }
    bool ok = check (tanhErr <= 2e-4,
                     "fastTanh (Pade 7/6) max error %.2e at x=%.3f over [-10, 10] "
                     "(want <= 2e-4, -74 dB)", tanhErr, tanhAt);
    ok = check (sinErr <= 1e-5,
                "fastSin (poly 9) max error %.2e at x=%.3f over [-1000, 1000] "
                "(want <= 1e-5)", sinErr, sinAt) && ok;
    return ok;
}

// ---------------------------------------------------------------------------
// Chaos and crush sanity (SPEC §5.2): outputs move, stay finite, and the
// crush mapping matches Daisy's Decimator (rate 1 / bits 0 = passthrough,
// rate 0 = hold of sr/1000 samples).

bool smokeChaosAndCrush()
{
    bool ok = true;
    struct DuffCase { double hz; int seconds; };
    const DuffCase duff[] = { { 2.0, 6 }, { 200.0, 1 } };
    for (const DuffCase& dc : duff)
    {
        for (double chaos : { 0.0, 1.0 })
        {
            char program[96];
            std::snprintf (program, sizeof (program),
                           "out = chaos_duffing(0, freq, %.1f);", chaos);
            auto v = makeVoice (program, 4u);
            if (! compiled (*v, "duffing"))
                return false;
            const std::vector<float> x = renderHeld (*v, dc.hz, dc.seconds * kOneSecond);
            const size_t half = x.size() / 2; // skip the start-up transient
            const double rms = rmsOf (x, half, x.size());
            const int zc = positiveZeroCrossings (x, half);
            const double seconds = 0.5 * dc.seconds;
            // chaos 0 is period-1 at hz; chaos 1 crosses wells irregularly.
            const int minZc = chaos == 0.0
                                ? static_cast<int> (0.5 * dc.hz * seconds)
                                : 1;
            ok = check (allFinite (x) && rms > 0.05 && zc >= minZc,
                        "chaos_duffing hz=%.0f chaos=%.0f moves: RMS %.3f, "
                        "%d crossings in %.1f s (want RMS > 0.05, >= %d)",
                        dc.hz, chaos, rms, zc, seconds, minZc) && ok;
        }
    }

    for (double rate : { 0.0, 0.5, 1.0 })
    {
        char program[96];
        std::snprintf (program, sizeof (program),
                       "out = chaos_lorenz(0, %.1f, 0);", rate);
        auto v = makeVoice (program, 5u);
        if (! compiled (*v, "lorenz"))
            return false;
        // rate 0 is a ~20 s orbit (0.05 Hz), so give it 10 s. Lorenz x only
        // crosses 0 when the orbit switches lobes: check movement instead.
        const int seconds = rate == 0.0 ? 10 : 1;
        const std::vector<float> x = renderHeld (*v, 110.0, seconds * kOneSecond);
        float lo = x[0], hi = x[0];
        for (float s : x)
        {
            lo = s < lo ? s : lo;
            hi = s > hi ? s : hi;
        }
        const double span = static_cast<double> (hi) - lo;
        ok = check (allFinite (x) && span > 0.02,
                    "chaos_lorenz rate=%.1f moves: span %.3f in %d s (want > 0.02), "
                    "RMS %.3f, %d lobe switches", rate, span, seconds,
                    rmsOf (x, 0, x.size()), positiveZeroCrossings (x)) && ok;
    }

    {
        auto v = makeVoice ("x = osc_saw(0, freq); out = (fx_crush(0, x, 0, 1) - x) * 1000000;", 6u);
        if (! compiled (*v, "crush passthrough"))
            return false;
        const std::vector<float> x = renderHeld (*v, 110.0, kOneSecond);
        ok = check (peakOf (x, 0, x.size()) == 0.0,
                    "fx_crush bits=0 rate=1 is an exact passthrough") && ok;
    }
    {
        auto v = makeVoice ("out = fx_crush(0, osc_sine(0, freq), 0, 0);", 6u);
        if (! compiled (*v, "crush rate 0"))
            return false;
        const std::vector<float> x = renderHeld (*v, 110.0, kOneSecond);
        int changes = 0;
        for (size_t i = 1; i < x.size(); ++i)
            if (x[i] != x[i - 1])
                ++changes;
        ok = check (changes >= 980 && changes <= 1001,
                    "fx_crush rate=0 holds 48 samples: %d updates in 1 s (want ~1000)",
                    changes) && ok;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// M6 fx_ott (SPEC §5.2): depth 0 is bit-exact passthrough, a quiet input
// comes out louder, a loud one doesn't grow, +24 dB stays finite.

bool smokeOtt()
{
    bool ok = true;
    {
        // osc_saw returns a float, so x * 0.5 is exact in the float the
        // wrapper sees: any difference is the compressor's.
        auto v = makeVoice ("x = osc_saw(0, freq) * 0.5; out = (fx_ott(0, x, 0) - x) * 1000000;", 7u);
        if (! compiled (*v, "fx_ott passthrough"))
            return false;
        const std::vector<float> x = renderHeld (*v, 110.0, kOneSecond);
        ok = check (peakOf (x, 0, x.size()) == 0.0, "fx_ott depth=0 is an exact passthrough") && ok;
    }
    struct Level { double gain; const char* what; };
    const Level levels[] = { { 0.01, "-40 dB" }, { 1.0, "0 dB" }, { 15.85, "+24 dB" } };
    for (const Level& lv : levels)
    {
        char dry[96], wet[128];
        std::snprintf (dry, sizeof (dry), "out = osc_saw(0, freq) * %.4f;", lv.gain);
        std::snprintf (wet, sizeof (wet), "out = fx_ott(0, osc_saw(0, freq) * %.4f, 1);", lv.gain);
        auto d = makeVoice (dry, 7u);
        auto w = makeVoice (wet, 7u);
        if (! compiled (*d, "fx_ott dry") || ! compiled (*w, "fx_ott wet"))
            return false;
        const std::vector<float> a = renderHeld (*d, 110.0, kOneSecond);
        const std::vector<float> b = renderHeld (*w, 110.0, kOneSecond);
        const size_t half = a.size() / 2; // after the 100 ms release settles
        const double dbIn = 20.0 * std::log10 (rmsOf (a, half, a.size()));
        const double dbOut = 20.0 * std::log10 (rmsOf (b, half, b.size()) + 1e-12);
        if (lv.gain < 0.1)
            ok = check (allFinite (b) && dbOut > dbIn + 6.0,
                        "fx_ott depth=1 lifts a quiet %s saw: %.1f -> %.1f dB RMS (want > +6 dB)",
                        lv.what, dbIn, dbOut) && ok;
        else if (lv.gain < 2.0)
            ok = check (allFinite (b) && dbOut <= dbIn,
                        "fx_ott depth=1 doesn't grow a loud %s saw: %.1f -> %.1f dB RMS",
                        lv.what, dbIn, dbOut) && ok;
        else
            ok = check (allFinite (b) && dbOut < dbIn,
                        "fx_ott depth=1 at %s input: finite, %.1f -> %.1f dB RMS, peak %.2f",
                        lv.what, dbIn, dbOut, peakOf (b, 0, b.size())) && ok;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Every bench case compiles and renders 0.25 s of finite output.

bool smokeAllFunctions()
{
    bool ok = true;
    int passed = 0;
    for (int ci = 0; ci < vibesynth_cli::kNumBenchCases; ++ci)
    {
        const vibesynth_cli::BenchCase& bc = vibesynth_cli::kBenchCases[ci];
        const PatchStrings ps = { "fb1 = 0; prev = 0;", bc.voice, bc.master };
        // Heap, not stack: EelVoice is ~175 KB since M7 (Windows' default stack is 1 MB).
        const auto vp = std::make_unique<EelVoice> (ps, kSampleRate, 8u);
        const auto mp = std::make_unique<EelMaster> (ps, kSampleRate);
        EelVoice& v = *vp;
        EelMaster& m = *mp;
        if (! v.isValid() || ! m.isValid())
        {
            ok = check (false, "%s compiles: %s %s", bc.name,
                        v.compileError().c_str(), m.compileError().c_str());
            continue;
        }
        double macros[MacroSmoother::kNumMacros] = {};
        fillMacros (macros, 0.5);
        v.noteOn (110.0, 45.0, 0.9);
        bool finite = true;
        for (int i = 0; i < kOneSecond / 4; ++i)
        {
            const float s = v.renderSample (110.0, 1.0, 45.0, 0.9, macros);
            float oL = 0.0f, oR = 0.0f;
            m.renderSample (s, s, oL, oR, macros);
            if (! isFiniteFloat (s) || ! isFiniteFloat (oL) || ! isFiniteFloat (oR))
                finite = false;
        }
        if (finite)
            ++passed;
        else
            ok = check (false, "%s renders finite output", bc.name);
    }
    return check (ok, "%d/%d lexicon cases compile and render finite",
                  passed, vibesynth_cli::kNumBenchCases) && ok;
}

// ---------------------------------------------------------------------------
// Default preset (Reese): init + voice + master, 2 s held at 55 Hz, then
// released. Output must be finite and sounding, the tail from gate-off +
// 0.5 s must stay below 0.01, and VoiceLifetime must free the voice.

bool smokePreset()
{
    const vibesynth::ParseResult pr = vibesynth::parsePatch (vibesynth::builtinPresetText (0));
    if (! check (pr.ok(), "default preset parses%s%s", pr.ok() ? "" : ": ", pr.error.c_str()))
        return false;
    const vibesynth::Patch& patch = pr.patch;
    const PatchStrings ps = { patch.init.c_str(), patch.voice.c_str(), patch.master.c_str() };
    const auto voicePtr = std::make_unique<EelVoice> (ps, kSampleRate, 0x1234u);
    const auto masterPtr = std::make_unique<EelMaster> (ps, kSampleRate);
    EelVoice& voice = *voicePtr;
    EelMaster& master = *masterPtr;
    bool ok = check (voice.isValid(), "%s voice compiles (init+voice)%s%s", patch.name.c_str(),
                     voice.isValid() ? "" : ": ", voice.compileError().c_str());
    ok = check (master.isValid(), "%s master compiles%s%s", patch.name.c_str(),
                master.isValid() ? "" : ": ", master.compileError().c_str()) && ok;
    if (! ok)
        return false;

    float targets[MacroSmoother::kNumMacros] = {};
    for (int m = 0; m < MacroSmoother::kNumMacros; ++m)
        targets[m] = static_cast<float> (patch.macroDefault (m));
    MacroSmoother smoother;
    smoother.init (kSampleRate);
    smoother.snapAll (targets);

    constexpr double kFreq = 55.0; // A1: reese bass register
    constexpr int kNoteOff = 2 * kOneSecond;
    constexpr int kTotal = 3 * kOneSecond;
    std::vector<float> outL ((size_t) kTotal, 0.0f);
    std::vector<float> outR ((size_t) kTotal, 0.0f);
    double macros[MacroSmoother::kNumMacros] = {};
    int freedAt = -1;

    vibesynth::VoiceLifetime lifetime;
    lifetime.noteOn();
    voice.noteOn (kFreq, 33.0, 0.9);
    for (int i = 0; i < kTotal; ++i)
    {
        if (i == kNoteOff)
            lifetime.noteOff();
        // Sweep m2 (cutoff) through the second half of the held note.
        if (i >= kOneSecond && i < kNoteOff)
            targets[1] = 0.5f + 0.4f * static_cast<float> (i - kOneSecond)
                                / static_cast<float> (kNoteOff - kOneSecond);
        smoother.step (targets);
        for (int m = 0; m < MacroSmoother::kNumMacros; ++m)
            macros[m] = smoother.current[m];
        const double gate = lifetime.gateHeld ? 1.0 : 0.0;
        const float v = voice.renderSample (kFreq, gate, 33.0, 0.9, macros);
        master.renderSample (v, v, outL[(size_t) i], outR[(size_t) i], macros);
        if (freedAt < 0 && lifetime.track (v, kSampleRate))
            freedAt = i;
    }

    ok = check (allFinite (outL) && allFinite (outR), "preset output finite (L and R)") && ok;
    const double peak = peakOf (outL, 0, (size_t) kNoteOff);
    const double rms = rmsOf (outL, 0, (size_t) kNoteOff);
    ok = check (peak >= 0.05 && peak <= 1.0 && rms > 0.01,
                "preset sounding while held: peak %.4f, RMS %.4f "
                "(want peak 0.05-1.0, RMS > 0.01)", peak, rms) && ok;
    const size_t tailStart = (size_t) (kNoteOff + kOneSecond / 2);
    const double tail = std::fmax (peakOf (outL, tailStart, outL.size()),
                                   peakOf (outR, tailStart, outR.size()));
    ok = check (tail < 0.01, "tail peak %.6f from gate-off + 0.5 s (want < 0.01)",
                tail) && ok;
    const double freedMs = freedAt < 0 ? -1.0
                         : 1000.0 * (freedAt - kNoteOff) / kSampleRate;
    ok = check (freedAt > kNoteOff && freedAt < kTotal,
                "VoiceLifetime frees the voice %.1f ms after gate-off", freedMs) && ok;
    return ok;
}

// ---------------------------------------------------------------------------
// M5 runaway guard (SPEC §7.1): EEL2 is built with
// NSEEL_LOOPFUNC_SUPPORT_MAXLEN=1024, so loop() and while() stop after 1024
// iterations on every JIT backend.

bool smokeLoopCap()
{
    struct Case { const char* what; const char* code; };
    const Case cases[] = {
        { "loop", "x = 0; loop(1000000, x += 1); out = x;" },
        { "while", "x = 0; while (x += 1; x < 1000000); out = x;" },
    };
    bool ok = true;
    for (const Case& c : cases)
    {
        auto v = makeVoice (c.code, 10u);
        if (! compiled (*v, c.what))
        {
            ok = false;
            continue;
        }
        const std::vector<float> x = renderHeld (*v, 110.0, 1);
        ok = check (x[0] == 1024.0f, "%s(1000000) runs %.0f iterations (want exactly 1024)",
                    c.what, static_cast<double> (x[0])) && ok;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Runtime guard reset (SPEC §7.2): resetState() zeroes every VM variable
// and mem[] (and the rack), without allocating.

bool smokeVoiceReset()
{
    auto v = makeVoice ("x += 1; mem[1000] += 1; out = x * 1000 + mem[1000] + osc_sine(0, 0) ;", 11u);
    if (! compiled (*v, "reset program"))
        return false;
    const std::vector<float> before = renderHeld (*v, 110.0, 3);
    v->resetState();
    const std::vector<float> after = renderHeld (*v, 110.0, 1);
    return check (before[2] == 3003.0f && after[0] == 1001.0f,
                  "resetState zeroes variables and mem[]: %.0f before, %.0f after "
                  "(want 3003, 1001)", static_cast<double> (before[2]),
                  static_cast<double> (after[0]));
}

// ---------------------------------------------------------------------------
// Output stage (SPEC §7.2): the limiter holds a +12 dB sine under the
// -0.3 dBFS ceiling, the DC blocker removes a 0.5 offset, and NaN input
// comes out as 0.

bool smokeOutputStage()
{
    vibesynth::OutputStage stage;
    stage.init (kSampleRate);
    double peak = 0.0;
    for (int i = 0; i < kOneSecond; ++i)
    {
        const float x = 4.0f * static_cast<float> (std::sin (6.283185307179586 * 110.0 * i / kSampleRate));
        float l = 0.0f, r = 0.0f;
        stage.process (x, -x, 1.0f, l, r);
        peak = std::fmax (peak, std::fmax (std::fabs (l), std::fabs (r)));
    }
    bool ok = check (peak <= vibesynth::PeakLimiter::kCeiling + 1e-6 && peak > 0.9,
                     "limiter: +12 dB sine peaks at %.4f (ceiling %.4f)", peak,
                     vibesynth::PeakLimiter::kCeiling);

    stage.init (kSampleRate);
    double tailMean = 0.0;
    for (int i = 0; i < 4 * kOneSecond; ++i)
    {
        float l = 0.0f, r = 0.0f;
        stage.process (0.5f, 0.5f, 1.0f, l, r);
        if (i >= 3 * kOneSecond)
            tailMean += l / kOneSecond;
    }
    ok = check (std::fabs (tailMean) < 1e-3, "DC blocker: 0.5 offset leaves mean %.6f after 3 s",
                tailMean) && ok;

    float l = 1.0f, r = 1.0f;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    stage.process (nan, 0.2f, 1.0f, l, r);
    ok = check (l == 0.0f && r == 0.0f, "NaN input comes out as 0 (got %g, %g)",
                static_cast<double> (l), static_cast<double> (r)) && ok;
    return ok;
}

// ---------------------------------------------------------------------------
// M6 --selftest, the preset gate: every built-in parses, round-trips
// exactly, passes the offline matrix, has its stored trim within 1 dB of
// the computed one (the plugin selects built-ins compile-only, SPEC 9.4),
// reports its CPU cost against its budget by voice mode (failing on it only
// with --strict-cpu, SPEC 4.2.1), builds a PatchProgram, and follows the authoring rules the few-shot
// examples rely on (a prompt, 4-8 macros, every macro labelled, and the
// code reads exactly the macros it labels).

// SPEC 9.4: a stored trim may differ from the computed one by up to 1 dB
// (FMA on arm64 and chaotic or crushed signals move the exact peak).
constexpr double kTrimToleranceDb = 1.0;

double trimDifferenceDb (double stored, double computed)
{
    return std::fabs (20.0 * std::log10 (stored / computed));
}

bool trimMatches (double stored, double computed)
{
    return trimDifferenceDb (stored, computed) <= kTrimToleranceDb;
}

// Selftest CPU check: median of 3 x 0.5 s (bench uses 7 x 1 s).
constexpr int kSelftestCostRuns = 3;
constexpr int kSelftestCostSamples = kOneSecond / 2;

bool selftestPreset (int index, bool strictCpu)
{
    using namespace vibesynth;
    const char* file = builtinPresetFile (index);
    const ParseResult pr = parsePatch (builtinPresetText (index));
    if (! check (pr.ok(), "%s parses%s%s", file, pr.ok() ? "" : ": ", pr.error.c_str()))
        return false;
    const Patch& patch = pr.patch;

    const std::string text = serializePatch (patch);
    const ParseResult again = parsePatch (text);
    bool ok = check (again.ok() && again.patch == patch && serializePatch (again.patch) == text,
                     "%s round-trips through serialize/parse", file);

    bool labelled = ! patch.macros.empty();
    for (const MacroSpec& m : patch.macros)
        labelled = labelled && ! m.label.empty();
    const int numMacros = static_cast<int> (patch.macros.size());
    ok = check (! patch.prompt.empty() && numMacros >= 4 && numMacros <= 8 && labelled,
                "%s has a prompt and %d labelled macros (want 4-8)", file, numMacros) && ok;
    const unsigned read = macrosRead (patch.init) | macrosRead (patch.voice) | macrosRead (patch.master);
    const unsigned listed = (1u << numMacros) - 1u;
    ok = check (read == listed, "%s reads exactly its labelled macros m1-m%d (read mask 0x%02x)",
                file, numMacros, read) && ok;

    const ValidationResult vr = validatePatch (patch, kSampleRate);
    if (! check (vr.ok(), "%s passes the offline test%s%s", file, vr.ok() ? "" : ": ",
                 vr.error.c_str()))
        return false;
    std::printf ("       %-16s peak %+5.1f dBFS, trim %+5.1f dB, worst config %.1f%% of real time, "
                 "matrix %3.0f ms (target < %.0f, report-only)\n",
                 patch.name.c_str(), 20.0 * std::log10 (vr.peak), 20.0 * std::log10 (vr.trim),
                 100.0 * vr.configCpu, vr.wallMs, patch.voiceMode == "mono" ? 600.0 : 300.0);

    ok = check (trimMatches (patch.trim, vr.trim),
                "%s stored trim %.6f matches the computed %.6f (%.3f dB apart, max %.0f)",
                file, patch.trim, vr.trim, trimDifferenceDb (patch.trim, vr.trim),
                kTrimToleranceDb) && ok;

    const double scale = validatorCpuScale();
    const vibesynth_cli::PresetCost cost = vibesynth_cli::measurePresetCost (
        patch, kSelftestCostRuns, kSelftestCostSamples, vibesynth::PatchProgram::kDefaultOversampling);
    const bool fits = cost.ok && cost.voiceNs <= patch.voiceBudgetNs() * scale
                      && cost.masterNs <= Patch::kMasterBudgetNs * scale;
    if (strictCpu)
        ok = check (fits, "%s fits its CPU budget: voice %.0f ns (%s, max %.0f), master %.0f ns (max %.0f)",
                    file, cost.voiceNs, patch.voiceMode.c_str(), patch.voiceBudgetNs() * scale,
                    cost.masterNs, Patch::kMasterBudgetNs * scale) && ok;
    else
        std::printf ("[INFO] %s CPU: voice %.0f ns (%s, budget %.0f), master %.0f ns (budget %.0f)%s\n",
                     file, cost.voiceNs, patch.voiceMode.c_str(), patch.voiceBudgetNs() * scale,
                     cost.masterNs, Patch::kMasterBudgetNs * scale,
                     ! cost.ok ? " - not measured" : fits ? "" : " - OVER (gates only with --strict-cpu)");

    // The plugin's built-in path: compile only, with the stored trim.
    std::string error;
    const auto prog = PatchProgram::build (patch, kSampleRate, error);
    ok = check (prog != nullptr, "%s builds a PatchProgram (10 voices + master)%s%s",
                file, prog != nullptr ? "" : ": ", error.c_str()) && ok;
    return ok;
}

int runSelftest (bool strictCpu)
{
    using namespace vibesynth;
    if (runningUnderRosetta())
        std::printf ("[NOTE] running under Rosetta: validator CPU limit and deadline x%.0f\n",
                     validatorCpuScale());
    std::printf (strictCpu ? "[NOTE] --strict-cpu: a preset over its CPU budget fails\n"
                           : "[NOTE] CPU budgets are report-only (gate with --strict-cpu)\n");
    vibesynth_cli::warmUp();
    bool ok = check (numBuiltinPresets() >= 15, "%d built-in presets (want >= 15)", numBuiltinPresets());
    int passed = 0;
    for (int i = 0; i < numBuiltinPresets(); ++i)
    {
        if (selftestPreset (i, strictCpu))
            ++passed;
        else
            ok = false;
    }
    ok = check (passed == numBuiltinPresets(), "%d/%d built-in presets pass", passed,
                numBuiltinPresets()) && ok;
    std::printf (ok ? "SELFTEST PASS\n" : "SELFTEST FAIL\n");
    return ok ? 0 : 1;
}

bool smokeBrokenPatches()
{
    using namespace vibesynth;
    bool ok = true;
    int rejected = 0;
    for (int i = 0; i < PulsarTestPatches::namedResourceListSize; ++i)
    {
        const std::string file = PulsarTestPatches::originalFilenames[i];
        const std::string prefix = "broken_", suffix = ".pulsar";
        if (file.rfind (prefix, 0) != 0 || file.size() <= prefix.size() + suffix.size())
            continue;
        const std::string want = file.substr (prefix.size(), file.size() - prefix.size() - suffix.size());

        int size = 0;
        const char* data = PulsarTestPatches::getNamedResource (PulsarTestPatches::namedResourceList[i], size);
        const std::string text (data != nullptr ? data : "", (size_t) (data != nullptr ? size : 0));

        PatchStatus status = PatchStatus::Ok;
        std::string reason;
        double wallMs = 0.0;
        const ParseResult pr = parsePatch (text);
        if (! pr.ok())
        {
            status = pr.status;
            reason = pr.error;
        }
        else
        {
            const ValidationResult vr = validatePatch (pr.patch, kSampleRate);
            status = vr.status;
            reason = vr.error;
            wallMs = vr.wallMs;
        }
        const bool good = status != PatchStatus::Ok && want == patchStatusName (status);
        if (good)
            ++rejected;
        ok = check (good, "%s rejected as %s (want %s) in %.0f ms: \"%s\"", file.c_str(),
                    patchStatusName (status), want.c_str(), wallMs, reason.c_str()) && ok;
        // SPEC 7.1: broken_slow is stopped by the 500 ms per-configuration
        // deadline in its first configuration, within a few samples of it.
        if (want == "slow")
        {
            const double deadlineMs = 500.0 * validatorCpuScale();
            ok = check (reason.rfind ("too slow: over", 0) == 0 && wallMs >= deadlineMs
                            && wallMs < deadlineMs + 100.0 * validatorCpuScale(),
                        "broken_slow stopped by the deadline after %.0f ms (want %.0f to %.0f)",
                        wallMs, deadlineMs, deadlineMs + 100.0 * validatorCpuScale()) && ok;
        }
    }
    ok = check (rejected == 11, "%d/11 broken test patches rejected", rejected) && ok;

    // Limits that need no file: total size and macro count.
    const ParseResult huge = parsePatch (std::string (Patch::kMaxFileBytes + 1, ' '));
    ok = check (huge.status == PatchStatus::TooLarge, "a 256 KB + 1 byte file is rejected: \"%s\"",
                huge.error.c_str()) && ok;
    std::string nine = "{\"version\": 1, \"macros\": [";
    for (int m = 0; m < 9; ++m)
        nine += std::string (m > 0 ? "," : "") + "{\"label\": \"x\", \"default\": 0.5}";
    nine += "], \"code\": {\"voice\": \"out = 0;\"}}";
    const ParseResult tooMany = parsePatch (nine);
    ok = check (tooMany.status == PatchStatus::BadField, "9 macros are rejected: \"%s\"",
                tooMany.error.c_str()) && ok;
    const ParseResult clamped = parsePatch (
        "{\"version\": 1, \"macros\": [{\"label\": \"a\", \"default\": 7}], \"code\": {\"voice\": \"out = 0;\"}}");
    ok = check (clamped.ok() && clamped.patch.macros[0].defaultValue == 1.0,
                "macro default 7 is clamped to 1") && ok;
    return ok;
}

// ---------------------------------------------------------------------------
// M6 loop-nesting check (SPEC §7.1): depth per case, functions counted at
// their call sites, comments/strings ignored; a nested patch is refused by
// PatchProgram::build too (restore/built-in path), and the two-level
// tests/patches/ok_nested.pulsar passes the whole offline test.

std::string testPatchText (const char* file)
{
    for (int i = 0; i < PulsarTestPatches::namedResourceListSize; ++i)
        if (std::strcmp (PulsarTestPatches::originalFilenames[i], file) == 0)
        {
            int size = 0;
            const char* data = PulsarTestPatches::getNamedResource (PulsarTestPatches::namedResourceList[i], size);
            return data != nullptr ? std::string (data, (size_t) size) : std::string();
        }
    return {};
}

bool smokeLoopNesting()
{
    using namespace vibesynth;
    struct Case { const char* code; int want; };
    const Case cases[] = {
        { "x = 1;", 0 },
        { "loop(4, x += 1);", 1 },
        { "loop(4, loop(4, x += 1));", 2 },
        { "loop(4, loop(4, loop(4, x += 1)));", 3 },
        { "while(loop(2, while(x < 1)); x < 2);", 3 },
        { "LOOP (2, Loop (2, lOOp (2, 0)));", 3 },
        { "while (x < 3) ( loop(2, loop(2, 0)); x += 1; );", 3 },
        { "// loop(1, loop(1, loop(1, 0)))\nloop(2, 0);", 1 },
        { "/* loop( loop( */ loop(2, 0); /* ) */", 1 },
        { "s = \"loop(loop(loop(\"; c = 'w('; loop(2, 0);", 1 },
        { "myloop(1); while_x = loop_n; x = loops(2);", 0 },
        { "function f() ( loop(2, x += 1) ); loop(2, f());", 2 },
        { "function f() ( loop(2, x += 1) ); loop(2, loop(2, f()));", 3 },
        { "function g() ( loop(2, 0) ); function f() ( loop(2, g()) ); loop(2, f());", 3 },
        { "function f() local(i) instance(j) ( loop(2, 0) ); loop(2, this.f());", 2 },
        { "function f() ( loop(2, loop(2, loop(2, 0))) ); x = 1;", 3 },
        { "function f() ( f() ); x = 1;", 99 },
    };
    bool ok = true;
    int passed = 0;
    for (const Case& c : cases)
    {
        const int got = loopNestingDepth (c.code);
        if (got == c.want)
            ++passed;
        else
            ok = check (false, "nesting depth %d (want %d): %s", got, c.want, c.code);
    }
    ok = check (ok, "loop-nesting check: %d/%d cases", passed, (int) (sizeof (cases) / sizeof (cases[0]))) && ok;

    const ParseResult nested = parsePatch (testPatchText ("broken_nested.pulsar"));
    std::string error;
    const auto prog = nested.ok() ? PatchProgram::build (nested.patch, kSampleRate, error) : nullptr;
    ok = check (nested.ok() && prog == nullptr && error.find ("nested too deep") != std::string::npos,
                "PatchProgram::build refuses broken_nested (restore/built-in path): \"%s\"",
                error.c_str()) && ok;

    const ParseResult two = parsePatch (testPatchText ("ok_nested.pulsar"));
    const ValidationResult vr = two.ok() ? validatePatch (two.patch, kSampleRate)
                                         : ValidationResult();
    ok = check (two.ok() && vr.ok() && vr.peak > 0.0,
                "ok_nested.pulsar (two levels, one inside a function) passes in %.0f ms%s%s",
                vr.wallMs, vr.ok() ? "" : ": ", (two.ok() ? vr.error : two.error).c_str()) && ok;
    return ok;
}

// ---------------------------------------------------------------------------
// M6: the processor's shutdown path (SPEC 6.6, 7.1). A juce::Thread
// validates broken_slow; after 30 ms the main thread raises the cancel flag
// and calls stopThread(), as ~VibeSynthAudioProcessor does. The per-sample
// cancel check must let it return within one sample (a few ms), well
// before the 500 ms deadline would.

class ValidateThread final : public juce::Thread
{
public:
    ValidateThread (const vibesynth::Patch& p, std::atomic<bool>& c)
        : juce::Thread ("Pulsar validate test"), patch (p), cancel (c) {}

    void run() override { result = vibesynth::validatePatch (patch, kSampleRate, &cancel); }

    vibesynth::ValidationResult result;

private:
    const vibesynth::Patch& patch;
    std::atomic<bool>& cancel;
};

bool smokeStopThread()
{
    using namespace vibesynth;
    const ParseResult pr = parsePatch (testPatchText ("broken_slow.pulsar"));
    if (! check (pr.ok(), "broken_slow parses"))
        return false;
    std::atomic<bool> cancel { false };
    ValidateThread thread (pr.patch, cancel);
    thread.startThread();
    juce::Thread::sleep (30);
    const bool stillRunning = thread.isThreadRunning();
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    cancel = true;
    thread.signalThreadShouldExit();
    const bool stopped = thread.stopThread (10000);
    const double stopMs = juce::Time::getMillisecondCounterHiRes() - t0;
    const double limitMs = 50.0 * validatorCpuScale();
    return check (stillRunning && stopped && thread.result.cancelled && stopMs < limitMs,
                  "stopThread returns %.1f ms after cancel while broken_slow validates "
                  "(want < %.0f ms; cancelled %d)", stopMs, limitMs, thread.result.cancelled ? 1 : 0);
}

// ---------------------------------------------------------------------------
// M6 master VoiceRack (SPEC 5.2): voice functions run in master with their
// own slots, bus_* still work there and bus_mono_low still returns 0 in a
// voice (bus_sat is stateless and allowed anywhere), and
// resetRacks()/resetState() reset both racks (the second run after a reset
// matches the first exactly).

bool smokeMasterRack()
{
    const PatchStrings ps = { "0;", "out = bus_mono_low(1, 1) + bus_mono_low_r();",
                              "x = osc_saw(0, 220) * 0.5; y = flt_lp(0, x, 0.4, 0.3);"
                              "c = chaos_lorenz(0, 0.6, 0);"
                              "outl = bus_sat(y + inl, 0.2); outr = y + c * 0.01;" };
    const auto v = std::make_unique<EelVoice> (ps, kSampleRate, 3u);
    const auto m = std::make_unique<EelMaster> (ps, kSampleRate);
    if (! check (v->isValid() && m->isValid(), "master program with voice functions compiles: %s",
                 m->compileError().c_str()))
        return false;
    double macros[MacroSmoother::kNumMacros] = {};
    fillMacros (macros, 0.5);

    auto run = [&] (std::vector<float>& l, std::vector<float>& r)
    {
        l.assign (4800, 0.0f);
        r.assign (4800, 0.0f);
        for (int i = 0; i < 4800; ++i)
            m->renderSample (0.0f, 0.0f, l[(size_t) i], r[(size_t) i], macros);
    };
    std::vector<float> l1, r1, l2, r2, l3, r3;
    run (l1, r1);
    m->resetRacks();
    run (l2, r2);
    m->resetState();
    run (l3, r3);

    bool ok = check (peakOf (r1, 0, r1.size()) > 0.05 && positiveZeroCrossings (r1) >= 15,
                     "osc_saw -> flt_lp runs in master (peak %.3f, %d crossings in 0.1 s at 220 Hz)",
                     peakOf (r1, 0, r1.size()), positiveZeroCrossings (r1));
    ok = check (peakOf (l1, 0, l1.size()) > 0.05, "bus_sat still works in master (peak %.3f)",
                peakOf (l1, 0, l1.size())) && ok;
    ok = check (l1 == l2 && r1 == r2 && l1 == l3 && r1 == r3,
                "resetRacks and resetState restart both master racks (osc, filter, chaos, bus)") && ok;
    const std::vector<float> vOut = renderHeld (*v, 110.0, 16);
    ok = check (v->isValid() && peakOf (vOut, 0, vOut.size()) == 0.0,
                "bus_mono_low returns 0 in a voice VM") && ok;
    return ok;
}

// ---------------------------------------------------------------------------
// M6 bpm/beat (SPEC 4.3): both VMs read the Transport they are given (120
// bpm, beat 0 by default); PatchPlayer advances beat per sample, follows
// the host's PPQ only while playing, runs on when stopped, and passes the
// per-sample beat to voices and master.

bool smokeTransport()
{
    using namespace vibesynth;
    bool ok = true;
    double macros[MacroSmoother::kNumMacros] = {};
    fillMacros (macros, 0.5);

    {
        const PatchStrings ps = { "ib = beat;", "out = bpm + beat * 1000 + ib * 10000;",
                                  "outl = bpm; outr = beat;" };
        EelVoice v (ps, kSampleRate, 1u);
        EelMaster m (ps, kSampleRate);
        if (! check (v.isValid() && m.isValid(), "bpm/beat programs compile: %s %s",
                     v.compileError().c_str(), m.compileError().c_str()))
            return false;
        const float fresh = v.renderSample (110.0, 1.0, 45.0, 0.9, macros);
        Transport t;
        t.bpm = 140.0;
        t.beat = 2.5;
        v.noteOn (110.0, 45.0, 0.9, t);
        t.beat = 3.0;
        const float played = v.renderSample (110.0, 1.0, 45.0, 0.9, macros, t);
        float l = 0.0f, r = 0.0f;
        m.renderSample (0.0f, 0.0f, l, r, macros, t);
        ok = check (fresh == 120.0f && played == 140.0f + 3000.0f + 25000.0f && l == 140.0f && r == 3.0f,
                    "voice and master read bpm/beat (default %.0f; voice %.0f want 28140; "
                    "master %.0f, %.1f)", static_cast<double> (fresh), static_cast<double> (played),
                    static_cast<double> (l), static_cast<double> (r)) && ok;
    }

    Patch patch;
    patch.name = "transport test";
    patch.voice = "out = 0;";
    std::string error;
    const auto prog = PatchProgram::build (patch, kSampleRate, error);
    if (! check (prog != nullptr, "transport test program builds %s", error.c_str()))
        return false;
    PatchPlayer player;
    float targets[PatchPlayer::kNumMacros] = {};
    player.setProgram (prog.get());
    player.prepare (kSampleRate, 512, targets);

    struct Ev { int samplePosition; const uint8_t* data; int numBytes; };
    std::vector<Ev> none;
    auto render = [&] (int total)
    {
        for (int done = 0; done < total; done += 512)
        {
            auto it = none.cbegin();
            player.renderChunk (std::min (512, total - done), targets, it, none.cend(), 0);
        }
    };
    player.setHostTransport (0.0, false, false, 0.0);
    render (kOneSecond);
    const Transport a = player.transport();
    player.setHostTransport (140.0, true, true, 16.0);
    render (4800);
    const Transport b = player.transport();
    player.setHostTransport (140.0, false, true, 99.0);
    render (4800);
    const Transport c = player.transport();
    ok = check (a.bpm == 120.0 && std::fabs (a.beat - 2.0) < 1e-9
                    && b.bpm == 140.0 && std::fabs (b.beat - (16.0 + 140.0 / 600.0)) < 1e-9
                    && std::fabs (c.beat - (16.0 + 2.0 * 140.0 / 600.0)) < 1e-9,
                "PatchPlayer beat: no host 120 bpm -> %.6f after 1 s (want 2); host plays from "
                "16 at 140 bpm -> %.6f after 0.1 s; stopped, runs on -> %.6f",
                a.beat, b.beat, c.beat) && ok;

    // Per-sample beat through the player: a held voice and the master each
    // step to 0.1 at beat 1 (sample 24000 at 120 bpm, from beat 0). M7: at
    // --os 1 exactly; at 2x the voice's step reaches the output through
    // the half-band decimator (~1.4 host samples of delay, plus ringing).
    for (int section = 0; section < 3; ++section)
    {
        Patch p;
        p.name = "beat step";
        p.voice = section != 1 ? "out = beat >= 1 ? 0.1 : 0;" : "out = 0;";
        p.master = section != 1 ? "" : "outl = outr = beat >= 1 ? 0.1 : 0;";
        const int os = section == 2 ? 2 : 1;
        const auto pr = PatchProgram::build (p, kSampleRate, error, os);
        if (! check (pr != nullptr, "beat step program builds %s", error.c_str()))
            return false;
        PatchPlayer pl;
        pl.setProgram (pr.get());
        pl.prepare (kSampleRate, 512, targets);
        const uint8_t noteOn[3] = { 0x90, 45, 100 };
        std::vector<Ev> events = { { 0, noteOn, 3 } };
        auto it = events.cbegin();
        int stepAt = -1;
        for (int start = 0; start < kOneSecond && stepAt < 0; start += 512)
        {
            pl.renderChunk (512, targets, it, events.cend(), start);
            for (int i = 0; i < 512 && stepAt < 0; ++i)
                if (pl.outL()[i] > 0.05f)
                    stepAt = start + i;
        }
        // 24000 * (120 / 2880000) rounds just below 1, so 24001 is exact too.
        const int late = os == 2 ? 3 : 1;
        ok = check (stepAt >= 24000 && stepAt <= 24000 + late,
                    "%s sees beat 1 at sample %d (want 24000 at 120 bpm, +%d for rounding%s)",
                    section == 0 ? "voice (1x)" : section == 1 ? "master" : "voice (2x)", stepAt, late,
                    os == 2 ? " and the decimator" : "") && ok;
    }
    return ok;
}

// M6 selftest rules: the 1 dB trim tolerance (SPEC 9.4) and the CPU budget
// by voice mode (SPEC 4.2.1).
bool smokeSelftestRules()
{
    using namespace vibesynth;
    bool ok = check (trimMatches (1.0, 0.9) && trimMatches (0.5, 0.56) && ! trimMatches (1.0, 0.88)
                         && ! trimMatches (0.5, 0.57),
                     "trim tolerance 1 dB: 0.92 and 0.98 dB apart match, 1.11 and 1.12 dB don't");
    Patch poly, mono;
    mono.voiceMode = "mono";
    ok = check (poly.voiceBudgetNs() == 300.0 && mono.voiceBudgetNs() == 1200.0,
                "voice budget by mode: poly %.0f ns, mono %.0f ns (want 300, 1200)",
                poly.voiceBudgetNs(), mono.voiceBudgetNs()) && ok;
    return ok;
}

int runSmoketest()
{
    bool ok = true;
    ok = smokeSine() && ok;
    ok = smokeTrig() && ok;
    ok = smokeStealFade() && ok;
    ok = smokeApproximations() && ok;
    ok = smokeChaosAndCrush() && ok;
    ok = smokeOtt() && ok;
    ok = smokeAllFunctions() && ok;
    ok = smokePreset() && ok;
    ok = smokeLoopCap() && ok;
    ok = smokeVoiceReset() && ok;
    ok = smokeOutputStage() && ok;
    ok = smokeBrokenPatches() && ok;
    ok = smokeLoopNesting() && ok;
    ok = smokeStopThread() && ok;
    ok = smokeMasterRack() && ok;
    ok = smokeTransport() && ok;
    ok = smokeSelftestRules() && ok;
    ok = vibesynth_cli::smokeDecimator() && ok;
    ok = vibesynth_cli::smokeAliasAB() && ok;
    ok = vibesynth_cli::smokeFilters() && ok;
    ok = vibesynth_cli::smokeVoiceModes() && ok;
    ok = vibesynth_cli::smokeApiReference() && ok;
    ok = vibesynth_cli::smokeLint() && ok;
    ok = vibesynth_cli::smokeResponseParser() && ok;
    ok = vibesynth_cli::smokePromptExamples() && ok;
    ok = vibesynth_cli::smokePromptBuilder() && ok;
    ok = vibesynth_cli::smokeClient() && ok;
    ok = vibesynth_cli::smokeSelfHeal() && ok;
    ok = vibesynth_cli::smokeCrlf() && ok;
    ok = vibesynth_cli::smokeTouchedNodes() && ok;
    ok = vibesynth_cli::smokeMeterHarvest() && ok;
    ok = vibesynth_cli::smokeNodeLayout() && ok;
    ok = vibesynth_cli::smokePatchLibrary() && ok;
    ok = vibesynth_cli::smokeKeyTest() && ok;
    ok = vibesynth_cli::smokeM10() && ok;
    ok = vibesynth_cli::smokeM11() && ok;
    ok = vibesynth_cli::smokeM12() && ok;
    ok = vibesynth_cli::smokeM12b() && ok;
    ok = vibesynth_cli::smokeMerge() && ok;
    ok = vibesynth_cli::smokeM13() && ok;
    std::printf (ok ? "SMOKETEST PASS\n" : "SMOKETEST FAIL\n");
    return ok ? 0 : 1;
}

} // namespace

// --validate file.pulsar: parse + the offline test at 48 kHz, as the
// plugin does on Load. Exit 0 when the patch passes, 1 when rejected.
int runValidate (const char* path)
{
    std::FILE* f = std::fopen (path, "rb");
    if (f == nullptr)
    {
        std::printf ("cannot open %s\n", path);
        return 2;
    }
    std::string text;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread (buf, 1, sizeof (buf), f)) > 0 && text.size() <= vibesynth::Patch::kMaxFileBytes)
        text.append (buf, n);
    std::fclose (f);

    const vibesynth::ParseResult pr = vibesynth::parsePatch (text);
    if (! pr.ok())
    {
        std::printf ("REJECTED (%s): %s\n", vibesynth::patchStatusName (pr.status), pr.error.c_str());
        return 1;
    }
    const vibesynth::ValidationResult vr = vibesynth::validatePatch (pr.patch, kSampleRate, nullptr,
                                                                     g_oversampling);
    if (! vr.ok())
    {
        std::printf ("REJECTED (%s): %s [%.0f ms]\n", vibesynth::patchStatusName (vr.status),
                     vr.error.c_str(), vr.wallMs);
        return 1;
    }
    // Peak (dBFS before trim) and DC per configuration, for balancing
    // macro ranges and seeing how close a patch is to a limit.
    std::printf ("    peak dBFS / DC  macros all 0    defaults       all 1\n");
    const int notes[] = { 28, 40, 55 };
    for (int n = 0; n < 3; ++n)
    {
        std::printf ("    note %d     ", notes[n]);
        for (int set = 0; set < 3; ++set)
            std::printf ("  %+5.1f / %.3f", 20.0 * std::log10 (std::fmax (vr.configPeak[n * 3 + set], 1e-9)),
                         vr.configDc[n * 3 + set]);
        std::printf ("\n");
    }
    std::printf ("PASS %s: peak %+.1f dBFS, trim %+.1f dB (%.6f), voice CPU %.2f%%, %.0f ms (voices %dx)\n",
                 pr.patch.name.c_str(), 20.0 * std::log10 (vr.peak), 20.0 * std::log10 (vr.trim),
                 vr.trim, 100.0 * vr.voiceCpu, vr.wallMs, g_oversampling);
    return 0;
}

int main (int argc, char** argvIn)
{
    // --os N anywhere after the mode (M7); removed before the mode parsing.
    std::vector<char*> args (argvIn, argvIn + argc);
    for (size_t i = 1; i + 1 < args.size(); ++i)
    {
        if (std::strcmp (args[i], "--os") != 0)
            continue;
        const std::string v = args[i + 1];
        if (v != "1" && v != "2")
        {
            std::printf ("bad --os %s (want 1 or 2)\n", v.c_str());
            return 2;
        }
        g_oversampling = v == "1" ? 1 : 2;
        args.erase (args.begin() + (std::ptrdiff_t) i, args.begin() + (std::ptrdiff_t) i + 2);
        break;
    }
    argc = static_cast<int> (args.size());
    char** argv = args.data();

    const bool smoke = argc == 2 && std::strcmp (argv[1], "--smoketest") == 0;
    const bool self = (argc == 2 || (argc == 3 && std::strcmp (argv[2], "--strict-cpu") == 0))
                      && std::strcmp (argv[1], "--selftest") == 0;
    const bool bench = argc == 2 && std::strcmp (argv[1], "--bench") == 0;
    const bool validate = argc == 3 && std::strcmp (argv[1], "--validate") == 0;
    const bool render = argc >= 4 && std::strcmp (argv[1], "--render") == 0;
    const bool batch = argc >= 3 && std::strcmp (argv[1], "--batch") == 0;
    const bool bake = argc == 4 && std::strcmp (argv[1], "--bake") == 0;  // M10
    // M11 (Edit.cpp): --change, --mutate, --change-batch.
    const bool change = argc >= 3 && std::strcmp (argv[1], "--change") == 0;
    const bool mutate = argc >= 3 && std::strcmp (argv[1], "--mutate") == 0;
    const bool changeBatch = argc >= 3 && std::strcmp (argv[1], "--change-batch") == 0;
    if (! smoke && ! self && ! bench && ! validate && ! render && ! batch && ! bake && ! change && ! mutate
        && ! changeBatch)
    {
        std::printf ("usage: vibesynth_cli (--smoketest | --selftest [--strict-cpu] | --bench | --validate file.pulsar\n"
                     "                     | --render <patch.pulsar | built-in> out.wav\n"
                     "                       [--sr HZ] [--seconds-tail S] [--bpm BPM] [--phrase default|riff]\n"
                     "                       [--mode patch|poly|mono] [--glide G] [--macro N=V]...\n"
                     "                     | --batch prompts.txt [--batch-limit N] [--out DIR] [--endpoint URL] [--model ID]\n"
                     "                       [--effort low|minimal|medium|none]\n"
                     "                     | --bake in.pulsar out.pulsar\n"
                     "                     | --change <in.pulsar | built-in> --node ID --say \"instruction\" --out out.pulsar\n"
                     "                     | --mutate <in.pulsar | built-in> --outdir DIR\n"
                     "                     | --change-batch cases.txt [--out DIR])\n"
                     "                     [--os 1|2]  (bench/validate/render voice oversampling, default 2)\n");
        return 2;
    }

    if (runningUnderRosetta())
        vibesynth::setValidatorCpuScale (kRosettaCpuScale);

    const char* mode = vibesynth::eelExecutionMode();
    std::printf ("mode: %s\n", mode);
    const bool jit = std::strcmp (mode, "portable") != 0;
    const bool initOk = vibesynth::ensureEelInitialised();
    const bool jitOk = initOk && vibesynth::isJitUsable();

    if (bench)
    {
        if (! jitOk)
        {
            std::printf ("EEL2 unavailable (init %d, JIT %d); nothing to bench\n",
                         initOk ? 1 : 0, jitOk ? 1 : 0);
            return 0;
        }
        return vibesynth_cli::runBench (g_oversampling);
    }
    if (validate)
        return jitOk ? runValidate (argv[2]) : 2;
    if (render)
        return jitOk ? vibesynth_cli::runRender (argc - 2, argv + 2, g_oversampling) : 2;
    if (batch)
        return jitOk ? vibesynth_cli::runBatch (argc - 2, argv + 2, g_oversampling) : 2;
    if (bake)
        return jitOk ? vibesynth_cli::runBake (argv[2], argv[3], g_oversampling) : 2;
    if (change)
        return jitOk ? vibesynth_cli::runChange (argc - 2, argv + 2, g_oversampling) : 2;
    if (mutate)
        return jitOk ? vibesynth_cli::runMutate (argc - 2, argv + 2, g_oversampling) : 2;
    if (changeBatch)
        return jitOk ? vibesynth_cli::runChangeBatch (argc - 2, argv + 2, g_oversampling) : 2;

    bool ok = check (jit, "execution mode is %s", mode);
    ok = check (initOk, "NSEEL_init") && ok;
    ok = check (jitOk, "JIT probe: executable pages available") && ok;
    if (! ok)
    {
        std::printf (self ? "SELFTEST FAIL\n" : "SMOKETEST FAIL\n");
        return 1;
    }
    return self ? runSelftest (argc == 3) : runSmoketest();
}
