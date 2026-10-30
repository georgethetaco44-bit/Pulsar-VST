// M4: headless test tool (SPEC §10.3). Shares the engine code with the
// plugin (links vibesynth_engine, no JUCE), so CI tests exactly the code the
// plugin runs. Modes:
//   --smoketest  gating in CI, exit 1 on any failure: the M3 sine checks
//                (440 Hz zero crossings, an isolated 220 Hz second voice,
//                JIT mode), trig semantics, the steal fade, the max error of
//                the tanh/sin approximations, chaos/crush sanity, every
//                bench case compiling and rendering finite,
//                and the M4 test preset (init + voice + master) held 2 s
//                then released.
//   --bench      report-only (always exit 0); see Bench.cpp.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <memory>
#include <vector>

#include "Bench.h"
#include "patch/PatchEngine.h"

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

bool check (bool ok, const char* fmt, ...)
{
    std::printf ("[%s] ", ok ? "PASS" : "FAIL");
    va_list args;
    va_start (args, fmt);
    std::vprintf (fmt, args);
    va_end (args);
    std::printf ("\n");
    return ok;
}

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
// Every bench case compiles and renders 0.25 s of finite output.

bool smokeAllFunctions()
{
    bool ok = true;
    int passed = 0;
    for (int ci = 0; ci < vibesynth_cli::kNumBenchCases; ++ci)
    {
        const vibesynth_cli::BenchCase& bc = vibesynth_cli::kBenchCases[ci];
        const PatchStrings ps = { "fb1 = 0; prev = 0;", bc.voice, bc.master };
        EelVoice v (ps, kSampleRate, 8u);
        EelMaster m (ps, kSampleRate);
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
// M4 test preset: init + voice + master, 2 s held at 55 Hz, then released.
// Output must be finite and sounding, the tail from gate-off + 0.5 s must
// stay below 0.01, and VoiceLifetime must free the voice.

bool smokePreset()
{
    using vibesynth::kTestPreset;
    EelVoice voice (kTestPreset, kSampleRate, 0x1234u);
    EelMaster master (kTestPreset, kSampleRate);
    bool ok = check (voice.isValid(), "test preset voice compiles (init+voice)%s%s",
                     voice.isValid() ? "" : ": ", voice.compileError().c_str());
    ok = check (master.isValid(), "test preset master compiles%s%s",
                master.isValid() ? "" : ": ", master.compileError().c_str()) && ok;
    if (! ok)
        return false;

    float targets[MacroSmoother::kNumMacros] = {
        0.4f, 0.5f, 0.45f, 0.35f, 0.5f, 0.5f, 0.5f, 0.5f
    };
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

    voice.noteOn (kFreq, 33.0, 0.9);
    for (int i = 0; i < kTotal; ++i)
    {
        if (i == kNoteOff)
            voice.lifetime.noteOff();
        // Sweep m2 (cutoff) through the second half of the held note.
        if (i >= kOneSecond && i < kNoteOff)
            targets[1] = 0.5f + 0.4f * static_cast<float> (i - kOneSecond)
                                / static_cast<float> (kNoteOff - kOneSecond);
        smoother.step (targets);
        for (int m = 0; m < MacroSmoother::kNumMacros; ++m)
            macros[m] = smoother.current[m];
        const double gate = voice.lifetime.gateHeld ? 1.0 : 0.0;
        const float v = voice.renderSample (kFreq, gate, 33.0, 0.9, macros);
        master.renderSample (v, v, outL[(size_t) i], outR[(size_t) i], macros);
        if (freedAt < 0 && voice.lifetime.track (v, kSampleRate))
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

int runSmoketest()
{
    bool ok = true;
    ok = smokeSine() && ok;
    ok = smokeTrig() && ok;
    ok = smokeStealFade() && ok;
    ok = smokeApproximations() && ok;
    ok = smokeChaosAndCrush() && ok;
    ok = smokeAllFunctions() && ok;
    ok = smokePreset() && ok;
    std::printf (ok ? "SMOKETEST PASS\n" : "SMOKETEST FAIL\n");
    return ok ? 0 : 1;
}

} // namespace

int main (int argc, char** argv)
{
    const bool smoke = argc == 2 && std::strcmp (argv[1], "--smoketest") == 0;
    const bool bench = argc == 2 && std::strcmp (argv[1], "--bench") == 0;
    if (! smoke && ! bench)
    {
        std::printf ("usage: vibesynth_cli (--smoketest | --bench)\n");
        return 2;
    }

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
        return vibesynth_cli::runBench();
    }

    bool ok = check (jit, "execution mode is %s", mode);
    ok = check (initOk, "NSEEL_init") && ok;
    ok = check (jitOk, "JIT probe: executable pages available") && ok;
    if (! ok)
    {
        std::printf ("SMOKETEST FAIL\n");
        return 1;
    }
    return runSmoketest();
}
