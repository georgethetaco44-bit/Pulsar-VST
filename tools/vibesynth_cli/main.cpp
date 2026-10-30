// M3 spike: headless test tool (SPEC §10.3). Shares the engine code with
// the plugin (links vibesynth_engine, no JUCE), so CI tests exactly the
// code the plugin runs. The only mode in M3 is --smoketest; --selftest /
// --render / --batch arrive with M6/M8.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/patch/PatchEngine.h"

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kNumSamples = 48000; // 1 s
constexpr double kTestFreq = 440.0;
constexpr double kIsoFreq = 220.0; // second VM: proves per-voice isolation
constexpr const char* kProgram = "out = osc_sine(0, freq);";

int countPositiveGoingZeroCrossings (const std::vector<float>& x)
{
    int n = 0;
    for (size_t i = 1; i < x.size(); ++i)
        if (x[i - 1] <= 0.0f && x[i] > 0.0f)
            ++n;
    return n;
}

bool impliedFreqOk (int zeroCrossings, double expected, double tolRel,
                    const char* label)
{
    // Whole-second render: positive-going crossings == cycles == Hz.
    const double implied = static_cast<double> (zeroCrossings);
    const double lo = expected * (1.0 - tolRel);
    const double hi = expected * (1.0 + tolRel);
    const bool ok = implied >= lo && implied <= hi;
    std::printf ("[%s] zero crossings: %d -> %.2f Hz implied "
                 "(expected %.1f Hz +/-%.0f%%)\n",
                 ok ? "PASS" : "FAIL", zeroCrossings, implied,
                 expected, tolRel * 100.0);
    std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    return ok;
}

} // namespace

int main (int argc, char** argv)
{
    if (argc != 2 || std::strcmp (argv[1], "--smoketest") != 0)
    {
        std::printf ("usage: vibesynth_cli --smoketest\n");
        return 2;
    }

    bool ok = true;
    std::printf ("mode: %s\n", vibesynth::eelExecutionMode());

    // Only a JIT mode proves the M3 exit test; portable is a fallback that
    // must be explicitly recorded (SPEC §13 M3), never silently accepted.
    const bool jit = std::strcmp (vibesynth::eelExecutionMode(), "portable") != 0;
    std::printf ("[%s] execution mode is %s\n", jit ? "PASS" : "FAIL",
                 vibesynth::eelExecutionMode());
    ok = ok && jit;

    if (! vibesynth::ensureEelInitialised())
    {
        std::printf ("[FAIL] NSEEL_init failed\n");
        return 1;
    }
    if (! vibesynth::isJitUsable())
    {
        std::printf ("[FAIL] JIT probe failed: exec pages denied; "
                     "refusing portable fallback in the spike\n");
        return 1;
    }

    vibesynth::EelVoice voice (kProgram, kSampleRate);
    if (! voice.isValid())
    {
        std::printf ("[FAIL] compile: %s\n", voice.compileError().c_str());
        return 1;
    }
    std::printf ("[PASS] compile: \"%s\"\n", kProgram);

    // Second voice at a different pitch, rendered sample-interleaved with
    // the first: proves each VM sees only its own custom-this context.
    vibesynth::EelVoice isoVoice (kProgram, kSampleRate);
    if (! isoVoice.isValid())
    {
        std::printf ("[FAIL] compile (isolation voice): %s\n",
                     isoVoice.compileError().c_str());
        return 1;
    }

    std::vector<float> out;
    std::vector<float> isoOut;
    out.reserve (kNumSamples);
    isoOut.reserve (kNumSamples);

    // Render, checking every sample for non-finite output (SPEC §11
    // bit-pattern tests, no std::isnan).
    for (int i = 0; i < kNumSamples; ++i)
    {
        const float s = voice.renderSample (kTestFreq, 1.0);
        const float t = isoVoice.renderSample (kIsoFreq, 1.0);
        out.push_back (s);
        isoOut.push_back (t);
        uint32_t b = 0, c = 0;
        static_assert (sizeof (b) == sizeof (s), "float must be 32-bit");
        std::memcpy (&b, &s, sizeof (s));
        std::memcpy (&c, &t, sizeof (t));
        // float exponent all-ones => Inf/NaN.
        if (((b >> 23) & 0xFFu) == 0xFFu || ((c >> 23) & 0xFFu) == 0xFFu)
        {
            std::printf ("[FAIL] non-finite sample at index %d\n", i);
            return 1;
        }
    }
    std::printf ("[PASS] no NaN/Inf in %d samples\n", kNumSamples);

    double sumSq = 0.0;
    for (float s : out)
        sumSq += static_cast<double> (s) * s;
    const double rms = std::sqrt (sumSq / out.size());
    const bool rmsOk = rms >= 0.636 && rms <= 0.778; // 0.7071 +/- 10%
    std::printf ("[%s] RMS %.5f (full-scale sine is 0.70711, want +/-10%%)\n",
                 rmsOk ? "PASS" : "FAIL", rms);
    ok = ok && rmsOk;

    ok = impliedFreqOk (countPositiveGoingZeroCrossings (out),
                        kTestFreq, 0.02, "440 Hz tone at 48 kHz") && ok;
    ok = impliedFreqOk (countPositiveGoingZeroCrossings (isoOut),
                        kIsoFreq, 0.02,
                        "per-voice isolation: 220 Hz voice unaffected") && ok;

    // Benchmark: best of several timed runs over one VM, then project
    // 8 voices at 48 kHz (ns/sample * 48000 * 8 / 1e7 = CPU %).
    constexpr int kReps = 5;
    double bestNs = 1e18;
    volatile float sink = 0.0f; // keep the loop from being optimised away
    for (int r = 0; r < kReps; ++r)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kNumSamples; ++i)
            sink += voice.renderSample (kTestFreq, 1.0);
        const auto t1 = std::chrono::steady_clock::now();
        const double ns =
            std::chrono::duration<double, std::nano> (t1 - t0).count()
            / kNumSamples;
        if (ns < bestNs)
            bestNs = ns;
    }
    const double cpuPct8 = bestNs * 48000.0 * 8.0 / 1e7;
    std::printf ("bench: %.1f ns/sample (one VM), projected 8-voice CPU "
                 "at 48 kHz: %.2f%%\n",
                 bestNs, cpuPct8);
    (void) sink;

    std::printf (ok ? "SMOKETEST PASS\n" : "SMOKETEST FAIL\n");
    return ok ? 0 : 1;
}
