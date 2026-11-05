// Prototype smoketests. See SmokePrototype.h.

#include "SmokePrototype.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "eel2/ns-eel.h"
#include "CliCheck.h"
#include "patch/LexiconLint.h"
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

// Writes a 32-bit float WAV, the frames given one after another.
void writeFloatWav (const juce::File& file, const std::vector<float>& samples)
{
    const uint32_t dataBytes = static_cast<uint32_t> (samples.size() * sizeof (float));
    juce::MemoryOutputStream out;
    auto tag = [&out] (const char* t) { out.write (t, 4); };
    tag ("RIFF");
    out.writeInt (static_cast<int> (36 + dataBytes));
    tag ("WAVE");
    tag ("fmt ");
    out.writeInt (16);
    out.writeShort (3);  // IEEE float
    out.writeShort (1);
    out.writeInt (48000);
    out.writeInt (48000 * 4);
    out.writeShort (4);
    out.writeShort (32);
    tag ("data");
    out.writeInt (static_cast<int> (dataBytes));
    out.write (samples.data(), dataBytes);
    file.replaceWithData (out.getData(), out.getDataSize());
}

// A sine of `amp` over `dc`, one frame.
void appendFrame (std::vector<float>& to, double amp, double dc)
{
    for (int n = 0; n < kBaseWtFrameSize; ++n)
        to.push_back (static_cast<float> (dc + amp * std::sin (2.0 * 3.14159265358979323846 * n / kBaseWtFrameSize)));
}

double mip0Peak (const LoadedWavetable& wt, int frame)
{
    const float* row = wt.data.data() + frame * kLoadedWtFrameStride;
    double peak = 0.0;
    for (int n = 0; n < kBaseWtFrameSize; ++n)
        peak = std::max (peak, static_cast<double> (std::fabs (row[n])));
    return peak;
}

double mip0Mean (const LoadedWavetable& wt, int frame)
{
    const float* row = wt.data.data() + frame * kLoadedWtFrameStride;
    double sum = 0.0;
    for (int n = 0; n < kBaseWtFrameSize; ++n)
        sum += static_cast<double> (row[n]);
    return sum / kBaseWtFrameSize;
}

bool smokeWavetableFiles()
{
    bool ok = true;
    const juce::TemporaryFile dirHandle;
    const juce::File dir = dirHandle.getFile();
    ok = check (dir.createDirectory().wasOk(), "wavetable test folder created in the temp directory") && ok;
    std::string reason;

    // A deliberately quiet file with DC, and frames at 4:2:1.
    {
        std::vector<float> s;
        appendFrame (s, 0.004, 0.1);
        appendFrame (s, 0.002, -0.05);
        appendFrame (s, 0.001, 0.02);
        const juce::File f = dir.getChildFile ("quiet.wav");
        writeFloatWav (f, s);
        const auto wt = buildLoadedWavetable (f.getFullPathName().toStdString(), reason);
        ok = check (wt != nullptr && wt->numFrames == 3, "quiet wavetable file loads (%s)", reason.c_str()) && ok;
        if (wt != nullptr && wt->numFrames == 3)
        {
            const double p0 = mip0Peak (*wt, 0), p1 = mip0Peak (*wt, 1), p2 = mip0Peak (*wt, 2);
            ok = check (std::fabs (p0 - kLoadedWtPeakLevel) < 0.005,
                        "quiet table (-48 dBFS) is raised so its peak reaches %.2f (got %.4f)",
                        static_cast<double> (kLoadedWtPeakLevel), p0) && ok;
            ok = check (std::fabs (p0 / p1 - 2.0) < 0.01 && std::fabs (p0 / p2 - 4.0) < 0.02,
                        "one gain for the whole table: frame levels stay 4:2:1 (got %.3f : %.3f : 1)",
                        p0 / p2, p1 / p2) && ok;
            const double dc = std::max ({ std::fabs (mip0Mean (*wt, 0)), std::fabs (mip0Mean (*wt, 1)),
                                          std::fabs (mip0Mean (*wt, 2)) });
            ok = check (dc < 1.0e-4, "DC is removed from every frame, even at 25x the signal (worst mean %.1e)", dc) && ok;

            // The mips are built after normalising, so they carry the same level.
            const float* mip3 = wt->data.data() + loadedWtMipOffset (3);
            double mip3Peak = 0.0;
            for (int n = 0; n < kBaseWtFrameSize >> 3; ++n)
                mip3Peak = std::max (mip3Peak, static_cast<double> (std::fabs (mip3[n])));
            ok = check (std::fabs (mip3Peak - p0) < 0.01, "mips are built after normalising (mip 3 peak %.4f, mip 0 %.4f)",
                        mip3Peak, p0) && ok;
        }
    }
    // A table already above full scale comes down, with NaN and Inf removed.
    {
        std::vector<float> s;
        appendFrame (s, 3.0, 0.0);
        s[10] = std::nanf ("");
        s[20] = HUGE_VALF;
        const juce::File f = dir.getChildFile ("hot.wav");
        writeFloatWav (f, s);
        const auto wt = buildLoadedWavetable (f.getFullPathName().toStdString(), reason);
        bool finite = wt != nullptr;
        if (wt != nullptr)
            for (float v : wt->data)
                finite = finite && std::isfinite (v);
        ok = check (finite && std::fabs (mip0Peak (*wt, 0) - kLoadedWtPeakLevel) < 0.005,
                    "hot table with NaN and Inf samples loads finite at the same peak level") && ok;
    }
    // Files that must be refused, each with a reason.
    {
        struct Bad { const char* name; std::vector<float> samples; const char* wantInReason; };
        std::vector<float> oneFrameSilent ((size_t) kBaseWtFrameSize, 0.0f);
        std::vector<float> dcOnly ((size_t) kBaseWtFrameSize, 0.5f);
        std::vector<float> badLength (3000, 0.5f);
        std::vector<Bad> bads = { { "silent.wav", oneFrameSilent, "silent" },
                                  { "dc_only.wav", dcOnly, "silent" },
                                  { "short.wav", badLength, "multiple of 2048" } };
        for (const Bad& b : bads)
        {
            const juce::File f = dir.getChildFile (b.name);
            writeFloatWav (f, b.samples);
            const auto wt = buildLoadedWavetable (f.getFullPathName().toStdString(), reason);
            ok = check (wt == nullptr && reason.find (b.wantInReason) != std::string::npos,
                        "%s is refused: %s", b.name, reason.c_str()) && ok;
        }
        std::vector<float> tooMany ((size_t) kBaseWtFrameSize * (kMaxWtFrames + 1), 0.1f);
        const juce::File big = dir.getChildFile ("too_many_frames.wav");
        writeFloatWav (big, tooMany);
        auto wt = buildLoadedWavetable (big.getFullPathName().toStdString(), reason);
        ok = check (wt == nullptr && reason.find ("frame count") != std::string::npos,
                    "257 frames is refused: %s", reason.c_str()) && ok;

        const juce::File junk = dir.getChildFile ("junk.wav");
        junk.replaceWithText ("this is not audio");
        wt = buildLoadedWavetable (junk.getFullPathName().toStdString(), reason);
        ok = check (wt == nullptr && ! reason.empty(), "a file that isn't a WAV is refused: %s", reason.c_str()) && ok;
    }
    // Cancelled: no table, no reason, so it isn't listed as a bad file.
    {
        const std::atomic<bool> cancelled { true };
        const auto wt = buildLoadedWavetable (dir.getChildFile ("quiet.wav").getFullPathName().toStdString(), reason, &cancelled);
        ok = check (wt == nullptr && reason.empty(), "a cancelled load gives no table and no error") && ok;
    }
    dir.deleteRecursively();

    const auto status = getWavetableFolderStatus();
    ok = check (! status.folderPath.empty() && ! status.loading,
                "the CLI has waited: the wavetable scan is finished before tests run (%s)", status.folderPath.c_str()) && ok;
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

// The output of `voiceCode` on a real VM at each given beat, one sample each.
// The init code fills mem[]; voiceCode writes out.
struct CurveVoice
{
    std::unique_ptr<EelVoice> voice;
    Transport t;
    double macros[8] = {};

    CurveVoice (const std::string& init, const std::string& code)
    {
        const PatchStrings ps = { init.c_str(), code.c_str(), "outl = inl; outr = inr;" };
        voice = std::make_unique<EelVoice> (ps, 96000.0, 11u);
    }
    bool valid() const { return voice->isValid(); }
    void noteOn (double beat)
    {
        t.beat = beat;
        voice->noteOn (110.0, 45.0, 1.0, t);
    }
    double at (double beat)
    {
        t.beat = beat;
        return voice->renderSample (110.0, 1.0, 45.0, 1.0, macros, t);
    }
    void* vm() { return voice->rackContext().vm; }
    // Writes the bits as they are; returns false if it didn't reach mem[].
    bool poke (int index, double value)
    {
        int valid = 0;
        EEL_F* ram = NSEEL_VM_getramptr_noalloc (static_cast<NSEEL_VMCTX> (vm()), static_cast<unsigned int> (index), &valid);
        if (ram == nullptr || valid < 1)
            return false;
        *ram = value;
        return std::memcmp (ram, &value, sizeof (value)) == 0;
    }
    // Settled value: long enough at one position for the 1 ms smoothing to end.
    // Walks there, because mod_curve_note takes a big move for a transport jump.
    double settled (double beat)
    {
        double v = 0.0;
        for (double b = t.beat + 0.2; b < beat; b += 0.2)
            v = at (b);
        for (int i = 0; i < 2400; ++i)
            v = at (beat);
        return v;
    }
};

// Patch code can't make these: EEL2's sqrt takes |x| first, so sqrt(-1) is 1, and
// its assignment flushes NaN and Inf to 0. So they go straight into mem[] and into
// the state's arguments.
const double kBadValues[] = { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(), 1.0e30, -1.0e30 };

double bendFormula (double t, double b)
{
    b = std::clamp (b, -8.0, 8.0);
    return std::fabs (b) < 0.01 ? t : (std::exp (b * t) - 1.0) / (std::exp (b) - 1.0);
}

std::string pointsInit (int base, const std::vector<std::array<double, 3>>& pts)
{
    std::string init;
    int at = base;
    for (const auto& p : pts)
        for (double v : p)
            init += "mem[" + std::to_string (at++) + "]=" + std::to_string (v) + ";";
    return init;
}

// The curve at known points, for both functions.
bool smokeCurveFormula (const char* fn)
{
    bool ok = true;
    const std::string f (fn);
    int worstCount = 0, total = 0;
    double worst = 0.0;
    const double bends[] = { 0.0, 0.005, -0.005, 0.01, 0.5, 2.0, -2.0, 5.0, -5.0, 8.0, -8.0, 50.0, -50.0 };
    for (double b : bends)
    {
        // Two segments: 0.2 -> 0.9 over x 0..0.5 with the bend under test, then flat.
        const std::string init = pointsInit (0, { { { 0.0, 0.2, b } }, { { 0.5, 0.9, 0.0 } }, { { 1.0, 0.9, 0.0 } } });
        CurveVoice cv (init, "out = " + f + "(0, 4.0, 0, 3);");
        if (! cv.valid())
            return check (false, "%s test patch compiles: %s", fn, cv.voice->compileError().c_str());
        cv.noteOn (0.0);
        for (double t : { 0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0 })
        {
            const double want = 0.2 + 0.7 * bendFormula (t, b);
            const double got = cv.settled (t * 2.0);  // 4 beats = x 0..1, so x 0.5 = beat 2
            const double err = std::fabs (got - want);
            worst = std::max (worst, err);
            ++total;
            if (err > 2.0e-5)
                ++worstCount;
        }
    }
    ok = check (worstCount == 0, "%s matches (exp(b t) - 1) / (exp(b) - 1) at %d known points, bends -50..50 clamped "
                "to +/-8, |b| < 0.01 linear (worst error %.1e)", fn, total, worst) && ok;

    // base: 2.7 rounds to 3; 2.2 rounds to 2. mem[0..2] holds a different curve.
    {
        const std::string init = "mem[0]=0.0; mem[1]=0.1; mem[2]=0.0;" "mem[3]=1.0; mem[4]=0.1; mem[5]=0.0;" "mem[6]=1.0; mem[7]=0.8; mem[8]=0.0;"
                                 "base = 0;";
        // Point 0 starts at base; base 3 reads the (1, 0.1) and (1, 0.8) pair, a jump at x = 1.
        CurveVoice up (init, "out = " + f + "(0, 4.0, 2.7, 2);");
        CurveVoice down (init, "out = " + f + "(0, 4.0, 2.2, 2);");
        ok = check (up.valid() && down.valid(), "%s base test compiles", fn) && ok;
        up.noteOn (0.0);
        down.noteOn (0.0);
        const double u = up.settled (0.0);
        const double d = down.settled (0.0);
        // Base 3: u <= x0 (1.0), so y of mem[4] = 0.1. Base 2: x0 = mem[2] = 0, y0 = mem[3] = 1.
        ok = check (std::fabs (u - 0.1) < 1.0e-5 && std::fabs (d - 1.0) < 1.0e-5,
                    "%s rounds base: 2.7 reads from 3 (%.4f, want 0.1000), 2.2 reads from 2 (%.4f, want 1.0000)", fn, u, d) && ok;
    }

    // Bad data: every output finite and 0..1 whatever mem[] and the arguments hold.
    {
        const std::string good = "mem[0]=0; mem[1]=0; mem[2]=0; mem[3]=0.5; mem[4]=1; mem[5]=2; mem[6]=1; mem[7]=0; mem[8]=0;";
        bool safe = true, poked = true;
        int cases = 0;
        const auto inRange = [] (double v) { return std::isfinite (v) && v >= 0.0 && v <= 1.0; };
        // Each field of each point, at each bad value, walked over 3 full cycles.
        for (int index = 0; index < 9; ++index)
            for (double bad : kBadValues)
            {
                CurveVoice cv (good, "out = " + f + "(0, 1.0, 0, 3);");
                cv.noteOn (0.0);
                poked = cv.poke (index, bad) && poked;
                ++cases;
                for (int i = 0; i < 1000; ++i)
                    safe = safe && inRange (cv.at (i * 0.003));
            }
        // Unsorted, out-of-range and all-equal points, through the VM.
        for (const char* init : { "mem[0]=0.8; mem[1]=999; mem[2]=50; mem[3]=0.2; mem[4]=-100; mem[5]=-50;", "memset(0, 0, 200);" })
            for (const char* call : { "(0, 1.0, 0, 2);", "(0, 0.25, 0, 32);", "(2, 0.0625, -0.4, 2);", "(0, 0, 65535, 32);",
                                      "(0, -1, 70000, -5);", "(99, 1, 65534.9, 2);", "(0, 0.000001, 0, 2);",
                                      "(1, pow(10, 30), -pow(10, 30), pow(10, 30));" })
            {
                CurveVoice cv (init, "out = " + f + call);
                safe = safe && cv.valid();
                cv.noteOn (0.0);
                ++cases;
                for (int i = 0; i < 1000; ++i)
                    safe = safe && inRange (cv.at (i * 0.003));
            }
        // Each argument at each bad value, on the state itself.
        CurveVoice host (good, "out = " + f + "(0, 1.0, 0, 3);");
        host.noteOn (0.0);
        host.at (0.0);
        for (int arg = 0; arg < 4; ++arg)
            for (double bad : kBadValues)
            {
                ModCurveState plain;
                ModCurveNoteState noted;
                noted.noteOn();
                ++cases;
                for (int i = 0; i < 300; ++i)
                {
                    double a[4] = { i * 0.003, 1.0, 0.0, 3.0 };
                    a[arg] = bad;
                    safe = safe && inRange (plain.process (host.vm(), a[0], a[1], a[2], a[3], 96000.0));
                    safe = safe && inRange (noted.process (host.vm(), a[0], a[1], a[2], a[3], 96000.0));
                }
            }
        ok = check (poked, "%s bad-data cases really hold NaN and Inf in mem[]", fn) && ok;
        ok = check (safe, "%s bounds/NaN: %d cases of NaN, Inf and +/-1e30 in every point field and argument, unsorted "
                    "points, out of range base: output finite and in 0..1", fn, cases) && ok;
    }
    // Only what the position needs is read: a NaN in a later point leaves the first segment playing.
    {
        CurveVoice cv ("mem[0]=0; mem[1]=0.25; mem[2]=0; mem[3]=0.5; mem[4]=0.75; mem[5]=0; mem[6]=0.75; mem[7]=0; mem[8]=0; mem[9]=1; mem[10]=1; mem[11]=0;",
                       "out = " + f + "(0, 1.0, 0, 4);");
        cv.noteOn (0.0);
        cv.at (0.0);
        ok = check (cv.poke (6, std::numeric_limits<double>::quiet_NaN()), "%s: NaN written to a later point", fn) && ok;
        const double early = cv.settled (0.25);
        const double late = cv.settled (0.9);
        ok = check (std::fabs (early - 0.5) < 1.0e-5 && late < 1.0e-5,
                    "%s: a NaN point only stops the curve when it is reached (%.4f before, %.4f after)", fn, early, late) && ok;
    }
    return ok;
}

// Walking from the last segment must give what scanning all the points gives,
// including unsorted x (running maximum) and vertical jumps.
bool smokeCurveAgainstFullScan()
{
    uint32_t rng = 0x2F6E2B1u;
    auto next = [&rng] {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return static_cast<double> (rng & 0xFFFFFFu) / 16777215.0;
    };
    double worst = 0.0;
    int compared = 0;
    for (int trial = 0; trial < 6; ++trial)
    {
        const int n = 2 + trial * 6;
        std::vector<std::array<double, 3>> pts;
        for (int i = 0; i < n; ++i)
        {
            const double x = trial % 2 == 0 ? (i + next() * 0.5) / n : next();  // odd trials: unsorted
            pts.push_back ({ x, next(), next() * 16.0 - 8.0 });
        }
        CurveVoice cv (pointsInit (0, pts), "out = mod_curve(0, 1.0, 0, " + std::to_string (n) + ");");
        if (! cv.valid())
            return check (false, "full-scan test patch compiles");
        cv.noteOn (0.0);
        for (int pass = 0; pass < 2; ++pass)
            for (int k = 0; k <= 40; ++k)
            {
                const double u = std::min (k / 40.0, 0.999);
                double x0 = pts[0][0];
                double want = pts[0][1];
                if (u > x0)
                {
                    want = pts[(size_t) n - 1][1];
                    for (int i = 0; i + 1 < n; ++i)
                    {
                        const double x1 = std::max (x0, pts[(size_t) i + 1][0]);
                        if (u <= x1)
                        {
                            const double span = x1 - x0;
                            want = span <= 1.0e-7
                                       ? pts[(size_t) i + 1][1]
                                       : pts[(size_t) i][1] + bendFormula ((u - x0) / span, pts[(size_t) i][2])
                                                                  * (pts[(size_t) i + 1][1] - pts[(size_t) i][1]);
                            break;
                        }
                        x0 = x1;
                    }
                }
                worst = std::max (worst, std::fabs (cv.settled (u + pass) - want));
                ++compared;
            }
    }
    return check (worst < 2.0e-4, "mod_curve reading only nearby points equals a full scan of 2-32 sorted and unsorted "
                  "points at %d positions (worst error %.1e)", compared, worst);
}

bool smokeCurveNote()
{
    bool ok = true;
    // y = x, 1 beat long, so the output reads the phase directly.
    const std::string init = "mem[0]=0; mem[1]=0; mem[2]=0; mem[3]=1; mem[4]=1; mem[5]=0;";
    CurveVoice note (init, "out = mod_curve_note(0, 1.0, 0, 2);");
    CurveVoice plain (init, "out = mod_curve(0, 1.0, 0, 2);");
    ok = check (note.valid() && plain.valid(), "mod_curve_note test patches compile") && ok;

    // A note-on at beat 3.5, halfway through mod_curve's cycle.
    note.noteOn (3.5);
    plain.noteOn (3.5);
    const double n0 = note.at (3.5);
    const double p0 = plain.at (3.5);
    ok = check (std::fabs (n0) < 1.0e-6 && std::fabs (p0 - 0.5) < 1.0e-5,
                "note-on at beat 3.5: mod_curve_note starts at phase 0 (%.4f), mod_curve follows the transport (%.4f)", n0, p0) && ok;
    const double n1 = note.settled (3.75);
    ok = check (std::fabs (n1 - 0.25) < 1.0e-4, "mod_curve_note counts beats from its note-on (a quarter beat in: %.4f)", n1) && ok;

    // A second note restarts it, without a click: the jump is smoothed.
    note.noteOn (3.75);
    const double first = note.at (3.75);
    double later = first;
    for (int i = 1; i < 960; ++i)
        later = note.at (3.75);
    ok = check (std::fabs (first - n1) < 0.01 && later < 0.001,
                "next note-on restarts it, smoothed: %.4f on the note-on sample, %.4f after 10 ms", first, later) && ok;

    // A host loop moves the transport back: the curve carries on.
    CurveVoice looped (init, "out = mod_curve_note(0, 4.0, 0, 2);");
    looped.noteOn (100.0);
    const double step = 0.001;
    double beat = 100.0, value = 0.0;
    for (int i = 0; i < 1000; ++i)
    {
        value = looped.at (beat);
        beat += step;
    }
    const double before = value;
    beat = 8.0;  // the locate
    for (int i = 0; i < 1000; ++i)
    {
        value = looped.at (beat);
        beat += step;
    }
    ok = check (std::fabs (before - 0.25) < 0.001 && std::fabs (value - 0.5) < 0.001,
                "a transport jump mid-note doesn't move mod_curve_note (%.4f, then %.4f after the same time again)", before, value) && ok;

    // Master: no notes, so it follows the transport, the same as mod_curve.
    {
        const PatchStrings ps = { init.c_str(), "out = osc_saw(0, freq);",
                                  "mem[0]=0; mem[1]=0; mem[2]=0; mem[3]=1; mem[4]=1; mem[5]=0;"
                                  "a = mod_curve_note(0, 1.0, 0, 2); b = mod_curve(0, 1.0, 0, 2);"
                                  "outl = a; outr = b;" };
        auto m = std::make_unique<EelMaster> (ps, 48000.0);
        ok = check (m->isValid(), "mod_curve_note compiles in master: %s", m->compileError().c_str()) && ok;
        double macros[8] = {};
        Transport t;
        double worst = 0.0, last = 0.0;
        for (int i = 0; i < 48000; ++i)
        {
            t.beat = 2.0 + i * 0.0001;
            float l = 0.0f, r = 0.0f;
            m->renderSample (0.0f, 0.0f, l, r, macros, t);
            worst = std::max (worst, static_cast<double> (std::fabs (l - r)));
            last = l;
        }
        ok = check (worst < 1.0e-6 && last > 0.5, "in master mod_curve_note follows the transport, the same as mod_curve "
                    "(max difference %.1e, ends at %.3f)", worst, last) && ok;
    }

    // Legato through the player: a second key held over the first has no trig.
    {
        Patch p;
        p.name = "Legato curve";
        p.voiceMode = "mono";
        p.init = init;
        // A tone at the curve's level (the output stage would block a bare curve as DC).
        p.voice = "out = mod_curve_note(0, 4.0, 0, 2) * osc_sine(0, 1000);";
        std::string err;
        auto prog = PatchProgram::build (p, 48000.0, err);
        ok = check (prog != nullptr, "legato test patch compiles: %s", err.c_str()) && ok;
        if (prog != nullptr)
        {
            PatchPlayer player;
            const float targets[PatchPlayer::kNumMacros] = {};
            player.setProgram (prog.get());
            player.prepare (48000.0, 512, targets);
            player.setVoiceControls (PatchPlayer::VoiceMode::Mono, 0.0);
            const uint8_t on1[] = { 0x90, 60, 100 };
            const uint8_t on2[] = { 0x90, 62, 100 };
            const uint8_t off[] = { 0x80, 62, 0 };
            const uint8_t off1[] = { 0x80, 60, 0 };
            auto chunk = [&player, &targets] (const uint8_t* msg)
            {
                TestMidiEvent ev { 0, msg, 3 };
                const TestMidiEvent* it = &ev;
                const TestMidiEvent* end = msg != nullptr ? &ev + 1 : &ev;
                player.renderChunk (512, targets, it, end, 0);
                double peak = 0.0;  // the second half: any 1 ms smoothing is over
                for (int i = 256; i < 512; ++i)
                    peak = std::max (peak, static_cast<double> (std::fabs (player.outL()[i])));
                return peak;
            };
            chunk (on1);
            double v = 0.0;
            for (int i = 0; i < 20; ++i)
                v = chunk (nullptr);
            const double held = v;
            const double legato = chunk (on2);
            ok = check (legato > held * 0.95 && held > 0.05,
                        "a legato note doesn't restart mod_curve_note (%.4f before, %.4f after)", held, legato) && ok;
            chunk (off);
            chunk (off1);
            for (int i = 0; i < 2; ++i)
                chunk (nullptr);
            const double retrig = chunk (on1);
            ok = check (retrig < held * 0.2, "a new note after all keys are up does restart it (%.4f)", retrig) && ok;
        }
    }
    return ok;
}

bool smokeUnison()
{
    bool ok = true;
    // Bounds and NaN through the VM, in both channels.
    {
        const char* cases[] = {
            "big = pow(10, 30); outl = osc_unison(0, big, big, big, big, big, big); outr = osc_unison_r(0);",
            "big = pow(10, 30); outl = osc_unison(1, -big, -big, -big, -big, -big, -big); outr = osc_unison_r(1);",
            "outl = osc_unison(99, 0, -99, 2, 0, 2, 2); outr = osc_unison_r(99);",
            "outl = osc_unison(0, 90000, 39, 1, 16, 1, 1); outr = osc_unison_r(0);",
            "outl = osc_unison(0, freq, 0, 0.5, 1 + 15 * (0.5 + 0.5 * sin(beat * 500)), rand(1), rand(1)); outr = osc_unison_r(0);",
        };
        double macros[8] = {};
        int passed = 0;
        for (const char* code : cases)
        {
            const PatchStrings ps = { "0;", code, "outl = inl; outr = inr;" };
            auto v = std::make_unique<EelVoice> (ps, 96000.0, 11u);
            if (! v->isValid())
                continue;
            Transport t;
            v->noteOn (440.0, 69.0, 0.9, t);
            bool good = true;
            for (int i = 0; i < 8192; ++i)
            {
                t.beat += t.beatStep (96000.0);
                float l = 0.0f, r = 0.0f;
                v->renderSample (440.0, 1.0, 69.0, 0.9, macros, t, 0.0, l, r);
                good = good && std::isfinite (l) && std::isfinite (r) && std::fabs (l) <= 4.0f && std::fabs (r) <= 4.0f;
            }
            if (good)
                ++passed;
        }
        ok = check (passed == 5, "osc_unison bounds: %d/5 extreme-argument cases through the VM finite and bounded", passed) && ok;

        // Each argument at each bad value, on the state itself.
        bool safe = true;
        int stateCases = 0;
        for (int arg = 0; arg < 6; ++arg)
            for (double bad : kBadValues)
            {
                UnisonOscState u;
                u.reset();
                ++stateCases;
                for (int i = 0; i < 2048; ++i)
                {
                    double a[6] = { 440.0, 0.0, 0.5, 4.0, 0.5, 0.5 };
                    if (i >= 512)
                        a[arg] = bad;
                    const double l = u.process (a[0], a[1], a[2], a[3], a[4], a[5], 96000.0);
                    const double r = u.lastR;
                    safe = safe && std::isfinite (l) && std::isfinite (r) && std::fabs (l) <= 4.0 && std::fabs (r) <= 4.0;
                }
            }
        ok = check (safe, "osc_unison NaN: %d cases of NaN, Inf and +/-1e30 in every argument, finite and bounded", stateCases) && ok;
    }

    // One voice is exactly the plain wavetable oscillator.
    {
        UnisonOscState u;
        WtOscState w;
        u.reset();
        double worst = 0.0;
        for (int i = 0; i < 4096; ++i)
        {
            const double a = u.process (220.0, 2.0, 0.4, 1.0, 0.0, 0.0, 96000.0);
            const double b = w.process (220.0, 2.0, 0.4, 96000.0);
            worst = std::max ({ worst, std::fabs (a - b), std::fabs (static_cast<double> (u.lastR) - b) });
        }
        ok = check (worst < 1.0e-5, "osc_unison with 1 voice equals osc_wt, both channels (max difference %.1e)", worst) && ok;
    }

    // hz is followed on every sample, not every 16.
    {
        UnisonOscState u;
        u.reset();
        u.process (1000.0, 0.0, 0.0, 1.0, 0.0, 0.0, 96000.0);
        const double p0 = u.phases[0];
        u.process (4000.0, 0.0, 0.0, 1.0, 0.0, 0.0, 96000.0);
        const double p1 = u.phases[0];
        ok = check (std::fabs ((p1 - p0) - 4000.0 / 96000.0) < 1.0e-9,
                    "osc_unison follows hz on the very next sample (phase step %.5f, want %.5f)", p1 - p0, 4000.0 / 96000.0) && ok;
    }

    // Silent voices skip the table but keep moving, and take no gain.
    {
        UnisonOscState u;
        u.reset();
        u.process (480.0, 0.0, 0.0, 4.0, 0.5, 0.5, 96000.0);
        const double start = u.phases[10];
        ok = check (u.activeVoices == 4, "4 voices: only 4 read the table (active %d of %d)", u.activeVoices, kMaxUnisonVoices) && ok;
        for (int i = 0; i < 160; ++i)  // 10 control intervals at 480 Hz: 0.8 cycles
            u.process (480.0, 0.0, 0.0, 4.0, 0.5, 0.5, 96000.0);
        double moved = u.phases[10] - start;
        moved -= std::floor (moved);
        ok = check (std::fabs (moved - 0.8) < 1.0e-6, "a silent voice's phase still advances (0.8 cycles in 160 samples: %.6f)", moved) && ok;
    }

    // No steps: moving spread, detune and voices fast adds no more jump to a
    // low sine than its own slope has. A stepped gain would show at once.
    {
        struct Move { const char* what; double depthDetune; double depthSpread; double depthVoices; };
        for (const Move& m : { Move { "spread", 0.0, 1.0, 0.0 }, Move { "detune", 1.0, 0.0, 0.0 }, Move { "voices", 0.0, 0.0, 1.0 } })
        {
            UnisonOscState steady, moving;
            steady.reset();
            moving.reset();
            double stepSteady = 0.0, stepMoving = 0.0, prevS = 0.0, prevM = 0.0;
            for (int i = 0; i < 48000; ++i)
            {
                const double lfo = 0.5 + 0.5 * std::sin (i * 2.0 * 3.14159265358979323846 * 40.0 / 96000.0);  // a fast 40 Hz sweep
                const double s = steady.process (110.0, 0.0, 0.0, 8.0, 0.5, 0.5, 96000.0);
                const double v = moving.process (110.0, 0.0, 0.0, 8.0 + m.depthVoices * (8.0 * lfo - 4.0), 0.5 + m.depthDetune * (lfo - 0.5),
                                                 0.5 + m.depthSpread * (lfo - 0.5), 96000.0);
                if (i > 2000)
                {
                    stepSteady = std::max (stepSteady, std::fabs (s - prevS));
                    stepMoving = std::max (stepMoving, std::fabs (v - prevM));
                }
                prevS = s;
                prevM = v;
            }
            ok = check (stepMoving <= stepSteady * 1.5, "osc_unison moving %s at 40 Hz stays step-free (largest sample-to-sample "
                        "change %.5f vs %.5f held)", m.what, stepMoving, stepSteady) && ok;
        }
    }

    // A note-on mid-ramp still ends on the right gains.
    {
        UnisonOscState u;
        u.reset();
        for (int i = 0; i < 40; ++i)
            u.process (220.0, 0.0, 0.0, 4.0, 0.5, 0.0, 96000.0);
        for (int i = 0; i < 5; ++i)
            u.process (220.0, 0.0, 0.0, 4.0, 0.5, 1.0, 96000.0);
        u.noteOn (1234u);
        for (int i = 0; i < 96000; ++i)
            u.process (220.0, 0.0, 0.0, 4.0, 0.5, 1.0, 96000.0);
        const double wantL = 0.5 * 1.4142135623730951 * std::cos (0.7853981633974483 * 2.0);  // hard right: voice 3
        ok = check (std::fabs (u.gainL[3] - wantL) < 1.0e-6 && std::fabs (u.stepL[3]) < 1.0e-12,
                    "osc_unison gains settle exactly after a note-on interrupts a ramp (voice 3 L gain %.6f)", u.gainL[3]) && ok;
    }

    // Unloaded table: the same as table 0.
    {
        UnisonOscState a, b;
        a.reset();
        b.reset();
        double worst = 0.0;
        for (int i = 0; i < 4096; ++i)
            worst = std::max (worst, std::fabs (a.process (220.0, 39.0, 0.3, 4.0, 0.5, 0.5, 96000.0)
                                                - b.process (220.0, getLoadedWavetable (39) == nullptr ? 0.0 : 39.0, 0.3, 4.0, 0.5, 0.5, 96000.0)));
        ok = check (worst < 1.0e-9, "osc_unison on a table not loaded yet plays table 0 (max difference %.1e)", worst) && ok;
    }
    return ok;
}

bool hasIssue (const LintResult& r, const char* text)
{
    return r.message().find (text) != std::string::npos;
}

bool smokeHalfStereo()
{
    bool ok = true;
    // Caught by the lint.
    struct Bad { const char* code; const char* want; };
    const Bad bad[] = {
        { "outl = osc_saw(0, freq);", "never outr" },
        { "outr = osc_saw(0, freq);", "never outl" },
        { "OutL = osc_saw(0, freq);", "never outr" },
        { "gate > 0 ? (outl += osc_saw(0, freq););", "never outr" },
        { "out = osc_saw(0, freq); outl = out; outr = out;", "writes out as well" },
        { "out = osc_saw(0, freq); outl = osc_saw(1, freq); outr = outl;", "writes out as well" },
        { "out = osc_saw(0, freq); outl = out;", "never outr" },
        { "out = osc_saw(0, freq); y = outl;", "without assigning" },
        { "function f(x) ( outl = x; ); f(osc_saw(0, freq));", "never outr" },
    };
    for (const Bad& b : bad)
    {
        const LintResult r = lintSection (b.code, "voice");
        ok = check (hasIssue (r, b.want), "lint catches \"%s\": %s", b.code, r.message().c_str()) && ok;
    }
    // Both messages tell the model what to write.
    {
        const std::string m = lintSection ("outl = 1;", "voice").message();
        ok = check (m.find ("outr = ") != std::string::npos && m.find ("voice line 1") != std::string::npos,
                    "the half-stereo lint message says where and what to add") && ok;
        const std::string n = lintSection ("x = 1;\nout = x; outl = x; outr = x;", "voice").message();
        ok = check (n.find ("voice line 2") != std::string::npos && n.find ("another name") != std::string::npos,
                    "the out-and-stereo lint message says where and what to do") && ok;
    }

    // Not caught: correct patches and things that only look like writes.
    const char* good[] = {
        "out = osc_saw(0, freq);",
        "out = osc_saw(0, freq); out *= 0.5;",
        "outl = osc_saw(0, freq); outr = outl;",
        "outl = osc_unison(0, freq, 0, 0, 4, 0.5, 0.5); outr = osc_unison_r(0);",
        "y = osc_saw(0, freq); outl = y; outr = y * 0.5;",
        "// outl = 1;\nout = osc_saw(0, freq);",
        "/* outr = 2; */ out = osc_saw(0, freq);",
        "function f(out) local(t) ( t = out * 2; out = t; out ); outl = f(1); outr = f(2);",
        "function f(x) local(out) ( out = x * 2; out ); outl = f(1); outr = f(2);",
        "without = 1; outlet = 2; router = 3; out = without + outlet + router;",
        "a.outl = 1; out = a.outl;",
        "outl = 1; outr = outl == 0 ? 1 : 0;",
    };
    for (const char* code : good)
    {
        const LintResult r = lintSection (code, "voice");
        std::string shown (code);
        std::replace (shown.begin(), shown.end(), '\n', ' ');
        ok = check (r.ok(), "lint accepts \"%s\": %s", shown.c_str(), r.message().c_str()) && ok;
    }
    // Writing them in init or master is a different thing.
    ok = check (lintSection ("outl = 0;", "init").ok() && lintSection ("outl = inl;", "master").ok(),
                "the voice output rule applies to voice code only") && ok;

    // Full patch path: the lint status, then the channel check.
    auto validate = [] (const char* voice, const char* master)
    {
        Patch p;
        p.name = "Channel test";
        p.voiceMode = "mono";
        p.voice = voice;
        p.master = master;
        return validatePatch (p, 48000.0);
    };
    const char* pass = "outl = inl; outr = inr;";
    const char* saw = "osc_saw(0, freq) * 0.5";
    {
        const ValidationResult r = validate ("outl = osc_saw(0, freq) * 0.5;", pass);
        ok = check (r.status == PatchStatus::LintError, "validator: half-written stereo voice fails as lint (%s)",
                    patchStatusName (r.status)) && ok;
    }
    struct Channel { std::string voice; const char* master; const char* name; bool rejected; const char* side; };
    const Channel channels[] = {
        { std::string ("outl = ") + saw + "; outr = 0;", pass, "voice right silent", true, "right" },
        { std::string ("outr = ") + saw + "; outl = 0;", pass, "voice left silent", true, "left" },
        { std::string ("out = ") + saw + ";", "outl = inl; outr = 0;", "master right silent", true, "right" },
        { std::string ("out = ") + saw + ";", "outr = inr; outl = 0;", "master left silent", true, "left" },
        { std::string ("y = ") + saw + "; outl = y; outr = y * m1;", pass, "right silent at macros 0 only", true, "right" },
        { std::string ("y = ") + saw + "; outl = y; outr = y * 0.01;", pass, "right 40 dB down", false, "" },
        { std::string ("y = ") + saw + "; outl = y; outr = -y;", pass, "polarity flip", false, "" },
        { std::string ("out = ") + saw + ";", pass, "plain mono", false, "" },
        { std::string ("out = ") + saw + ";", "", "mono, default master", false, "" },
    };
    for (const Channel& c : channels)
    {
        const ValidationResult r = validate (c.voice.c_str(), c.master);
        if (c.rejected)
            ok = check (r.status == PatchStatus::OneChannel && r.error.find (c.side) == 0,
                        "validator rejects %s: %s", c.name, r.error.c_str()) && ok;
        else
            ok = check (r.ok(), "validator accepts %s (%s %s)", c.name, patchStatusName (r.status), r.error.c_str()) && ok;
    }
    {
        const ValidationResult r = validate ("outl = 0; outr = 0;", pass);
        ok = check (r.status == PatchStatus::Silent, "both channels silent is still \"silent\" (%s)", patchStatusName (r.status)) && ok;
    }
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
    ok = smokeWavetableFiles() && ok;
    ok = smokeUnisonSweeping() && ok;
    ok = smokeUnison() && ok;
    ok = smokeCurveFormula ("mod_curve") && ok;
    ok = smokeCurveFormula ("mod_curve_note") && ok;
    ok = smokeCurveAgainstFullScan() && ok;
    ok = smokeCurveNote() && ok;
    ok = smokeHalfStereo() && ok;
    return ok;
}

int runWavetableLoadTest()
{
    using Clock = std::chrono::steady_clock;
    const auto ms = [] (Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli> (b - a).count(); };
    std::printf ("folder: %s\n", getWavetableFolderStatus().folderPath.c_str());

    // Open and close at once, as a host scanning plugins does.
    double worstBegin = 0.0, worstEnd = 0.0;
    for (int i = 0; i < 20; ++i)
    {
        const auto t0 = Clock::now();
        beginWavetableLoad();
        const auto t1 = Clock::now();
        endWavetableLoad();
        const auto t2 = Clock::now();
        worstBegin = std::max (worstBegin, ms (t0, t1));
        worstEnd = std::max (worstEnd, ms (t1, t2));
    }
    std::printf ("20 x open then close at once: begin worst %.3f ms, stop and join worst %.3f ms, %d tables so far\n",
                 worstBegin, worstEnd, getNumLoadedWavetables());

    // Close in the middle of a table.
    beginWavetableLoad();
    std::this_thread::sleep_for (std::chrono::milliseconds (150));
    const auto stop0 = Clock::now();
    endWavetableLoad();
    const auto stop1 = Clock::now();
    const int partial = getNumLoadedWavetables();
    std::printf ("close 150 ms into the scan: stop and join %.3f ms, %d tables published\n", ms (stop0, stop1), partial);

    // Open again: the scan finishes, in the background, with the rest.
    const auto start = Clock::now();
    beginWavetableLoad();
    const auto started = Clock::now();
    int seen = getNumLoadedWavetables();
    bool tablesAppeared = false;
    while (getWavetableFolderStatus().loading)
    {
        const int now = getNumLoadedWavetables();
        tablesAppeared = tablesAppeared || now > seen;
        seen = now;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    const auto done = Clock::now();
    endWavetableLoad();

    const auto status = getWavetableFolderStatus();
    size_t frames = 0;
    for (const auto& info : status.loaded)
        frames += (size_t) info.numFrames;
    std::printf ("reopen: begin %.3f ms; remaining tables loaded in %.0f ms while the caller ran; appeared one by one: %s\n",
                 ms (start, started), ms (start, done), tablesAppeared ? "yes" : "no");
    std::printf ("result: %d tables, %d listed, %zu frames, %d skipped\n", getNumLoadedWavetables(),
                 (int) status.loaded.size(), frames, (int) status.skipped.size());
    for (const auto& sk : status.skipped)
        std::printf ("  skipped %s\n", sk.c_str());

    // Nothing lost or doubled across the cancelled runs.
    bool inOrder = (int) status.loaded.size() == getNumLoadedWavetables();
    for (size_t i = 0; i < status.loaded.size(); ++i)
        inOrder = inOrder && status.loaded[i].tableNumber == 8 + (int) i;
    std::printf ("table numbers run 8.. without gaps or repeats: %s\n", inOrder ? "yes" : "NO");
    return inOrder ? 0 : 1;
}

} // namespace vibesynth_cli
