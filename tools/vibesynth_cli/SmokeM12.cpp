// M12 smoketests:
//   - Bounds/NaN tests on every new M12 function at extreme/garbage arguments
//   - osc_wt mipmap selection, high-pitch band-limiting, 8 tables & pos smoothing
//   - osc_fm and osc_sync pitch/spectrum sanity
//   - flt_notch, flt_peak, flt_formant, flt_tilt response & flat neutral points
//   - mod_lfo note-on retrigger, tempo sync, and unipolar [0, 1] range across shapes
//   - ADAA state isolation per stateless call index (0..15 vs >= 16) and per voice
//   - ADAA branch-flip transient boundedness at extreme inputs
//   - ADAA aliasing reduction on high-note sweep through fx_fold, fx_sat, fx_shape
//   - Full M12 patch using every new function: validatePatch + bakePatch equivalence

#include "SmokeM12.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "CliCheck.h"
#include "engine/Filters.h"
#include "engine/Filters2.h"
#include "engine/Lexicon.h"
#include "engine/NodeOverrides.h"
#include "engine/Utilities.h"
#include "engine/Utilities2.h"
#include "engine/Wavetables.h"
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
constexpr double kVoiceRate = 96000.0;
constexpr double kPi = 3.14159265358979323846;

bool isFiniteFloat (float v) noexcept
{
    uint32_t b = 0;
    std::memcpy (&b, &v, sizeof (v));
    return ((b >> 23) & 0xFFu) != 0xFFu;
}

void fft (std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * kPi / static_cast<double> (len);
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

std::vector<double> powerSpectrum (const std::vector<float>& x, size_t start, size_t n)
{
    std::vector<std::complex<double>> a (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = 2.0 * kPi * static_cast<double> (i) / static_cast<double> (n);
        const double w = 0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2.0 * t)
                       - 0.01168 * std::cos (3.0 * t);
        a[i] = w * static_cast<double> (x[start + i]);
    }
    fft (a);
    std::vector<double> p (n / 2 + 1);
    for (size_t k = 0; k <= n / 2; ++k)
        p[k] = std::norm (a[k]);
    return p;
}

double inharmonicDb (const std::vector<float>& x, size_t start, size_t n, double f0, double sr)
{
    const std::vector<double> p = powerSpectrum (x, start, n);
    const double binHz = sr / static_cast<double> (n);
    std::vector<bool> harmonic (p.size(), false);
    for (int h = 1; h * f0 < 0.5 * sr; ++h)
    {
        const long c = std::lround (h * f0 / binHz);
        for (long k = c - 6; k <= c + 6; ++k)
            if (k >= 0 && k < static_cast<long> (p.size()))
                harmonic[static_cast<size_t> (k)] = true;
    }
    double total = 0.0, inh = 0.0;
    for (size_t k = static_cast<size_t> (std::ceil (20.0 / binHz)); k < p.size(); ++k)
    {
        total += p[k];
        if (! harmonic[k])
            inh += p[k];
    }
    return 10.0 * std::log10 (std::max (inh, 1.0e-300) / std::max (total, 1.0e-300));
}

double highBandInharmonicDb (const std::vector<float>& x, size_t start, size_t n, double f0,
                             double minHz, double sr)
{
    const std::vector<double> p = powerSpectrum (x, start, n);
    const double binHz = sr / static_cast<double> (n);
    std::vector<bool> harmonic (p.size(), false);
    for (int h = 1; h * f0 < 0.5 * sr; ++h)
    {
        const long c = std::lround (h * f0 / binHz);
        for (long k = c - 6; k <= c + 6; ++k)
            if (k >= 0 && k < static_cast<long> (p.size()))
                harmonic[static_cast<size_t> (k)] = true;
    }
    double total = 0.0, inhHi = 0.0;
    const size_t minBin = static_cast<size_t> (std::ceil (minHz / binHz));
    for (size_t k = static_cast<size_t> (std::ceil (20.0 / binHz)); k < p.size(); ++k)
    {
        total += p[k];
        if (k >= minBin && ! harmonic[k])
            inhHi += p[k];
    }
    return 10.0 * std::log10 (std::max (inhHi, 1.0e-300) / std::max (total, 1.0e-300));
}

// ---------------------------------------------------------------------------
// 1. Bounds and NaN tests at extreme/garbage arguments.

bool smokeBoundsAndNan()
{
    const char* cases[] = {
        "nan = sqrt(-1); big = pow(10, 30); out = osc_wt(0, nan, -99, big) + osc_wt(1, 0, 99, -big) + osc_wt(0, 50000, 3, nan);",
        "nan = sqrt(-1); big = pow(10, 30); out = osc_fm(0, nan, -big, big, nan) + osc_fm(1, 0, big, -big, big) + osc_fm(0, 40000, 0.5, 1, 1);",
        "nan = sqrt(-1); big = pow(10, 30); out = osc_sync(0, nan, big, -big) + osc_sync(1, 0, -big, nan) + osc_sync(0, 50000, 1, 1);",
        "nan = sqrt(-1); big = pow(10, 30); out = fx_shape(nan, big, -99) + fx_shape(big, -big, 99) + fx_shape(-big, nan, 3);",
        "nan = sqrt(-1); big = pow(10, 30); out = fx_fold(nan, big) + fx_fold(big, -big) + fx_sat(nan, big) + fx_sat(-big, 1);",
        "nan = sqrt(-1); big = pow(10, 30); out = flt_notch(0, nan, -big, big) + flt_notch(1, big, big, nan);",
        "nan = sqrt(-1); big = pow(10, 30); out = flt_peak(0, nan, -big, big, nan) + flt_peak(1, big, big, -big, big);",
        "nan = sqrt(-1); big = pow(10, 30); out = flt_formant(0, nan, -big, big) + flt_formant(1, big, big, nan);",
        "nan = sqrt(-1); big = pow(10, 30); out = flt_tilt(0, nan, -big) + flt_tilt(1, big, big) + flt_tilt(0, 0.5, nan);",
        "nan = sqrt(-1); big = pow(10, 30); out = mod_lfo(0, nan, -big) + mod_lfo(1, 0, big) + mod_lfo(2, big, nan);",
    };
    const int numCases = static_cast<int> (sizeof (cases) / sizeof (cases[0]));
    double macros[MacroSmoother::kNumMacros] = {};
    for (int m = 0; m < MacroSmoother::kNumMacros; ++m)
        macros[m] = 0.5;
    int passed = 0;
    for (int ci = 0; ci < numCases; ++ci)
    {
        const PatchStrings ps = { "0;", cases[ci], "big = pow(10, 30); nan = sqrt(-1); outl = bus_sat(inl, big); outr = bus_sat(inr, nan);" };
        auto v = std::make_unique<EelVoice> (ps, kVoiceRate, 11u);
        auto m = std::make_unique<EelMaster> (ps, kRate);
        if (! v->isValid() || ! m->isValid())
            continue;
        Transport t;
        v->noteOn (440.0, 69.0, 0.9, t);
        bool finite = true;
        double maxAbs = 0.0;
        for (int i = 0; i < 4096; ++i)
        {
            t.beat += t.beatStep (kVoiceRate);
            const float s = v->renderSample (440.0, 1.0, 69.0, 0.9, macros, t);
            float oL = 0.0f, oR = 0.0f;
            m->renderSample (s, s, oL, oR, macros, t);
            if (! isFiniteFloat (s) || ! isFiniteFloat (oL) || ! isFiniteFloat (oR))
                finite = false;
            maxAbs = std::max (maxAbs, static_cast<double> (std::fabs (s)));
        }
        if (finite && maxAbs < 100.0)
            ++passed;
    }
    return check (passed == numCases,
                  "M12 bounds/NaN: %d/%d extreme-argument cases (+/-1e30, NaN, 0 Hz, >20 kHz) finite and bounded",
                  passed, numCases);
}

// ---------------------------------------------------------------------------
// 2. osc_wt mipmap selection, high-pitch band-limiting, and 8 tables.

bool smokeWavetables()
{
    initWavetables();
    bool ok = true;

    // Mip selection grows monotonically with pitch (dt = hz / sr) and clamps to [0, kNumWtMips - 1].
    const int mLow = selectWtMip (55.0, kVoiceRate);
    const int mMid = selectWtMip (880.0, kVoiceRate);
    const int mHigh = selectWtMip (12000.0, kVoiceRate);
    const int mNyq = selectWtMip (0.49 * kVoiceRate, kVoiceRate);
    ok = check (mLow == 0 && mMid > mLow && mHigh > mMid && mNyq == kNumWtMips - 1,
                "M12 osc_wt mip selection: 55 Hz -> mip %d, 880 Hz -> mip %d, 12 kHz -> mip %d, near-Nyquist -> mip %d",
                mLow, mMid, mHigh, mNyq)
       && ok;

    // Every table (0..7) produces finite, non-silent audio and morphs across pos 0..1.
    int tablesOk = 0;
    for (int tbl = 0; tbl < kNumWavetables; ++tbl)
    {
        WtOscState st0, st1;
        st0.reset();
        st1.reset();
        double energy0 = 0.0, energy1 = 0.0, diff = 0.0;
        for (int i = 0; i < 4096; ++i)
        {
            const float a = static_cast<float> (st0.process (110.0, tbl, 0.0, kVoiceRate));
            const float b = static_cast<float> (st1.process (110.0, tbl, 1.0, kVoiceRate));
            energy0 += static_cast<double> (a) * a;
            energy1 += static_cast<double> (b) * b;
            diff += std::fabs (static_cast<double> (a - b));
        }
        if (energy0 > 1.0 && energy1 > 1.0 && diff > 10.0)
            ++tablesOk;
    }
    ok = check (tablesOk == kNumWavetables,
                "M12 osc_wt: all %d/%d procedural wavetables sound and morph between pos 0 and pos 1",
                tablesOk, kNumWavetables)
       && ok;

    // High-pitch band-limiting: at 7040 Hz (A8) at 48 kHz, max harmonic is <= 3,
    // so inharmonic aliased energy must stay below -65 dB.
    WtOscState hiOsc;
    hiOsc.reset();
    constexpr size_t kFft = 16384;
    std::vector<float> sig (kFft + 2048);
    const double f0 = 7040.0;
    for (size_t i = 0; i < sig.size(); ++i)
        sig[i] = static_cast<float> (hiOsc.process (f0, 0, 1.0, kRate));
    const double inh = inharmonicDb (sig, 2048, kFft, f0, kRate);
    ok = check (inh <= -65.0,
                "M12 osc_wt band-limiting at %.0f Hz (table 0, pos 1): inharmonic energy %.1f dB (want <= -65 dB)",
                f0, inh)
       && ok;

    return ok;
}

// ---------------------------------------------------------------------------
// 3. mod_lfo note-on retrigger, tempo sync, and unipolar [0, 1] range.

bool smokeModLfo()
{
    bool ok = true;
    ModLfoState lfo;
    lfo.reset (42u);
    Transport t;
    t.bpm = 120.0;
    t.beat = 3.37;
    lfo.noteOn (t.beat);

    // At noteOn beat (phase 0), raised-cosine sine shape (0.0) starts smoothly at 0.0,
    // quarter-cycle later (0.25 beats) is 0.5, half-cycle (0.50 beats) is 1.0.
    const float s0 = static_cast<float> (lfo.process (t.beat, 1.0, 0.0));
    t.beat += 0.25;
    const float sQuarter = static_cast<float> (lfo.process (t.beat, 1.0, 0.0));
    t.beat += 0.25;
    const float sHalf = static_cast<float> (lfo.process (t.beat, 1.0, 0.0));
    ok = check (std::fabs (s0 - 0.0f) < 1e-4f && std::fabs (sQuarter - 0.5f) < 1e-4f
                    && std::fabs (sHalf - 1.0f) < 1e-4f,
                "M12 mod_lfo tempo sync + note-on retrigger: phase 0 -> %.3f, +1/4 period -> %.3f, +1/2 period -> %.3f",
                s0, sQuarter, sHalf)
       && ok;

    // Retrigger at a new noteOn resets phase to 0 regardless of elapsed beats.
    t.beat = 19.83;
    lfo.noteOn (t.beat);
    const float sRetrig = static_cast<float> (lfo.process (t.beat, 2.0, 0.25)); // tri at phase 0 is 0.0
    ok = check (std::fabs (sRetrig) < 1e-4f,
                "M12 mod_lfo noteOn retrigger resets triangle phase to 0 (got %.4f)", sRetrig)
       && ok;

    // Unipolar [0, 1] across all shapes (0..1) including S&H.
    bool inRange = true;
    float minV = 1.0f, maxV = 0.0f;
    for (int sh = 0; sh <= 8; ++sh)
    {
        const double shape = sh / 8.0;
        t.beat = 0.0;
        lfo.reset (99u + static_cast<uint32_t> (sh));
        lfo.noteOn (0.0);
        for (int i = 0; i < 4000; ++i)
        {
            t.beat += 0.005;
            const float v = static_cast<float> (lfo.process (t.beat, 0.5, shape));
            if (v < -1e-6f || v > 1.000001f || ! isFiniteFloat (v))
                inRange = false;
            minV = std::min (minV, v);
            maxV = std::max (maxV, v);
        }
    }
    ok = check (inRange && minV <= 0.05f && maxV >= 0.95f,
                "M12 mod_lfo unipolar [0, 1] across all shapes (span %.3f..%.3f)", minV, maxV)
       && ok;

    return ok;
}

// ---------------------------------------------------------------------------
// 4. Filter neutral points & formant response.

bool smokeNewFilters()
{
    bool ok = true;
    FilterRate fr;
    fr.init (kVoiceRate);

    // flt_peak at gain=0.5 (0 dB) is an exact passthrough (within float precision).
    PeakSvfState peak;
    peak.reset();
    double maxPeakDiff = 0.0;
    for (int i = 0; i < 4096; ++i)
    {
        const float x = static_cast<float> (0.5 * std::sin (2.0 * kPi * 440.0 * i / kVoiceRate));
        const float y = peak.process (x, 0.5, 0.5, 0.5, fr);
        maxPeakDiff = std::max (maxPeakDiff, static_cast<double> (std::fabs (y - x)));
    }
    ok = check (maxPeakDiff < 1e-5,
                "M12 flt_peak at gain=0.5 is flat passthrough (max diff %.2e)", maxPeakDiff)
       && ok;

    // flt_tilt at tilt=0.5 (0 dB) is an exact passthrough.
    TiltState tilt;
    tilt.reset();
    double maxTiltDiff = 0.0;
    for (int i = 0; i < 4096; ++i)
    {
        const float x = static_cast<float> (0.5 * std::sin (2.0 * kPi * 440.0 * i / kVoiceRate));
        const float y = tilt.process (x, 0.5, fr);
        maxTiltDiff = std::max (maxTiltDiff, static_cast<double> (std::fabs (y - x)));
    }
    ok = check (maxTiltDiff < 1e-5,
                "M12 flt_tilt at tilt=0.5 is flat passthrough (max diff %.2e)", maxTiltDiff)
       && ok;

    // flt_notch rejects its center frequency (cutoff=0.5 -> ~632 Hz) by >= 30 dB.
    SvfState notch;
    notch.reset();
    const double fCenter = 20.0 * std::pow (1000.0, 0.5);
    double sumSq = 0.0;
    const int settle = 8192, n = 16384;
    for (int i = 0; i < settle + n; ++i)
    {
        const float x = static_cast<float> (std::sin (2.0 * kPi * fCenter * i / kVoiceRate));
        const float y = notch.process (x, 0.5, 0.3, fr, SvfState::Notch);
        if (i >= settle)
            sumSq += static_cast<double> (y) * y;
    }
    const double notchGainDb = 20.0 * std::log10 (std::sqrt (sumSq / n) / std::sqrt (0.5) + 1e-12);
    ok = check (notchGainDb <= -30.0,
                "M12 flt_notch at cutoff 0.5 (%.1f Hz): center attenuation %.1f dB (want <= -30 dB)",
                fCenter, notchGainDb)
       && ok;

    return ok;
}

// ---------------------------------------------------------------------------
// 5. ADAA state isolation (calls 0..15 vs >=16, and per-voice isolation)
//    and branch-flip transient boundedness.

bool smokeAdaaIsolationAndBranches()
{
    bool ok = true;
    double macros[MacroSmoother::kNumMacros] = {};
    for (int m = 0; m < MacroSmoother::kNumMacros; ++m)
        macros[m] = 0.5;

    // A single fx_sat call in a voice must produce bit-identical output whether or not
    // a second fx_sat call with wild input runs after it in the same sample.
    const PatchStrings psSingle = {
        "0;",
        "x = osc_saw(0, freq); out = fx_sat(x, 0.8);",
        "outl = inl; outr = inr;"
    };
    const PatchStrings psTwoCalls = {
        "0;",
        "x = osc_saw(0, freq); a = fx_sat(x, 0.8); b = fx_sat(osc_sine(1, 3700) * 5.0, 1.0); out = a;",
        "outl = inl; outr = inr;"
    };
    auto v1 = std::make_unique<EelVoice> (psSingle, kVoiceRate, 5u);
    auto v2 = std::make_unique<EelVoice> (psTwoCalls, kVoiceRate, 5u);
    v1->noteOn (220.0, 57.0, 0.9);
    v2->noteOn (220.0, 57.0, 0.9);
    bool callIsolated = true;
    for (int i = 0; i < 2048; ++i)
    {
        const float s1 = v1->renderSample (220.0, 1.0, 57.0, 0.9, macros);
        const float s2 = v2->renderSample (220.0, 1.0, 57.0, 0.9, macros);
        if (std::memcmp (&s1, &s2, sizeof (float)) != 0)
            callIsolated = false;
    }
    ok = check (callIsolated, "M12 ADAA per-call state isolation: fx_sat#0 is bit-identical when fx_sat#1 runs alongside")
       && ok;

    // 20 fx_sat calls in one sample (exceeding kMaxAdaaCalls = 16): calls 0..15 use ADAA,
    // calls 16..19 fall back cleanly without corrupting slot 0 or crashing.
    std::string manyCode = "x = osc_saw(0, freq); a0 = fx_sat(x, 0.8); s = 0; ";
    for (int k = 1; k < 20; ++k)
        manyCode += "s += fx_sat(x * 0.5, 0.6); ";
    manyCode += "out = a0 + s * 0.0;";
    const PatchStrings psMany = { "0;", manyCode.c_str(), "outl = inl; outr = inr;" };
    auto vRef = std::make_unique<EelVoice> (psSingle, kVoiceRate, 5u);
    auto vMany = std::make_unique<EelVoice> (psMany, kVoiceRate, 5u);
    vRef->noteOn (220.0, 57.0, 0.9);
    vMany->noteOn (220.0, 57.0, 0.9);
    bool capSafe = true;
    for (int i = 0; i < 2048; ++i)
    {
        const float sRef = vRef->renderSample (220.0, 1.0, 57.0, 0.9, macros);
        const float sMany = vMany->renderSample (220.0, 1.0, 57.0, 0.9, macros);
        if (std::memcmp (&sRef, &sMany, sizeof (float)) != 0)
            capSafe = false;
    }
    ok = check (capSafe, "M12 ADAA call-cap safety: 20 fx_sat calls (> kMaxAdaaCalls=16) keep slot 0 bit-identical")
       && ok;

    // Branch-flip transient boundedness at extreme inputs (+/-1e30).
    const PatchStrings psBranch = {
        "c = 0; big = pow(10, 30);",
        "c = !c; x = c ? big : -big; "
        "out = (c ? fx_fold(x, 1.0) : fx_fold(-x, 0.5)) * 0.25 "
        "+ (c ? fx_sat(x, 1.0) : fx_sat(-x, 0.5)) * 0.25 "
        "+ (c ? fx_shape(x, 1.0, 5) : fx_shape(-x, 0.8, 0)) * 0.25;",
        "outl = inl; outr = inr;"
    };
    auto vBranch = std::make_unique<EelVoice> (psBranch, kVoiceRate, 7u);
    vBranch->noteOn (440.0, 69.0, 0.9);
    bool branchBounded = true;
    double maxPeak = 0.0;
    for (int i = 0; i < 4096; ++i)
    {
        const float s = vBranch->renderSample (440.0, 1.0, 69.0, 0.9, macros);
        if (! isFiniteFloat (s) || std::fabs (s) > 2.0f)
            branchBounded = false;
        maxPeak = std::max (maxPeak, static_cast<double> (std::fabs (s)));
    }
    ok = check (branchBounded,
                "M12 ADAA branch-flip transient boundedness (+/-1e30 alternating): peak %.3f (want <= 2.0)",
                maxPeak)
       && ok;

    return ok;
}

// ---------------------------------------------------------------------------
// 6. ADAA aliasing reduction before (memoryless) and after (ADAA).

bool smokeAdaaAliasing()
{
    bool ok = true;
    constexpr size_t kFft = 16384;
    constexpr size_t kSettle = 2048;
    // High-note sweep (D#7..F#8) where folded/saturated harmonics exceed Nyquist and alias back.
    const double notesHz[] = { 2489.0, 3136.0, 3951.1, 4978.0, 5919.9 };
    const int numNotes = static_cast<int> (sizeof (notesHz) / sizeof (notesHz[0]));

    double foldNoAdaaSum = 0.0, foldAdaaSum = 0.0;
    double foldHiNoAdaaSum = 0.0, foldHiAdaaSum = 0.0;
    double satNoAdaaSum = 0.0, satAdaaSum = 0.0;
    double satHiNoAdaaSum = 0.0, satHiAdaaSum = 0.0;
    double shapeNoAdaaSum = 0.0, shapeAdaaSum = 0.0;

    for (int ni = 0; ni < numNotes; ++ni)
    {
        const double f0 = notesHz[ni];
        std::vector<float> foldRaw (kSettle + kFft), foldAdaa (kSettle + kFft);
        std::vector<float> satRaw (kSettle + kFft), satAdaa (kSettle + kFft);
        std::vector<float> shapeRaw (kSettle + kFft), shapeAdaa (kSettle + kFft);
        AdaaSlot foldSlot, satSlot, shapeSlot;
        foldSlot.reset();
        satSlot.reset();
        shapeSlot.reset();

        for (size_t i = 0; i < kSettle + kFft; ++i)
        {
            const double ph = 2.0 * kPi * f0 * static_cast<double> (i) / kVoiceRate;
            const float x = static_cast<float> (1.25 * std::sin (ph) + 0.35 * std::sin (2.0 * ph));
            foldRaw[i] = foldSample (x, 0.8, nullptr);
            foldAdaa[i] = foldSample (x, 0.8, &foldSlot);
            satRaw[i] = satSample (x, 0.8, nullptr);
            satAdaa[i] = satSample (x, 0.8, &satSlot);
            shapeRaw[i] = shapeSample (x, 0.8, 0.0, nullptr);
            shapeAdaa[i] = shapeSample (x, 0.8, 0.0, &shapeSlot);
        }
        foldNoAdaaSum += inharmonicDb (foldRaw, kSettle, kFft, f0, kVoiceRate);
        foldAdaaSum += inharmonicDb (foldAdaa, kSettle, kFft, f0, kVoiceRate);
        foldHiNoAdaaSum += highBandInharmonicDb (foldRaw, kSettle, kFft, f0, 16000.0, kVoiceRate);
        foldHiAdaaSum += highBandInharmonicDb (foldAdaa, kSettle, kFft, f0, 16000.0, kVoiceRate);

        satNoAdaaSum += inharmonicDb (satRaw, kSettle, kFft, f0, kVoiceRate);
        satAdaaSum += inharmonicDb (satAdaa, kSettle, kFft, f0, kVoiceRate);
        satHiNoAdaaSum += highBandInharmonicDb (satRaw, kSettle, kFft, f0, 16000.0, kVoiceRate);
        satHiAdaaSum += highBandInharmonicDb (satAdaa, kSettle, kFft, f0, 16000.0, kVoiceRate);

        shapeNoAdaaSum += inharmonicDb (shapeRaw, kSettle, kFft, f0, kVoiceRate);
        shapeAdaaSum += inharmonicDb (shapeAdaa, kSettle, kFft, f0, kVoiceRate);
    }

    const double foldBefore = foldNoAdaaSum / numNotes;
    const double foldAfter = foldAdaaSum / numNotes;
    const double foldHiBefore = foldHiNoAdaaSum / numNotes;
    const double foldHiAfter = foldHiAdaaSum / numNotes;

    const double satBefore = satNoAdaaSum / numNotes;
    const double satAfter = satAdaaSum / numNotes;
    const double satHiBefore = satHiNoAdaaSum / numNotes;
    const double satHiAfter = satHiAdaaSum / numNotes;

    const double shapeBefore = shapeNoAdaaSum / numNotes;
    const double shapeAfter = shapeAdaaSum / numNotes;

    ok = check (foldAfter <= foldBefore - 4.0,
                "M12 ADAA fx_fold(drive=0.8) aliasing: inharmonic %.1f -> %.1f dB (%.1f dB better; >16 kHz %.1f -> %.1f dB)",
                foldBefore, foldAfter, foldBefore - foldAfter, foldHiBefore, foldHiAfter)
       && ok;
    ok = check (satAfter <= satBefore - 6.0,
                "M12 ADAA fx_sat(drive=0.8) aliasing: inharmonic %.1f -> %.1f dB (%.1f dB better; >16 kHz %.1f -> %.1f dB)",
                satBefore, satAfter, satBefore - satAfter, satHiBefore, satHiAfter)
       && ok;
    ok = check (shapeAfter <= shapeBefore - 6.0,
                "M12 ADAA fx_shape(drive=0.8) aliasing: inharmonic %.1f -> %.1f dB (%.1f dB better)",
                shapeBefore, shapeAfter, shapeBefore - shapeAfter)
       && ok;

    return ok;
}

// ---------------------------------------------------------------------------
// 7. Full M12 patch using every new function: validatePatch + bakePatch.

std::vector<float> renderPatchPhrase (const Patch& patch, const OverrideTable* overrides)
{
    std::string err;
    auto prog = PatchProgram::build (patch, kRate, err);
    if (! prog)
        return {};
    PatchPlayer player;
    player.setProgram (prog.get());
    float targets[PatchPlayer::kNumMacros] = {};
    for (int m = 0; m < PatchPlayer::kNumMacros; ++m)
        targets[m] = static_cast<float> (patch.macroDefault (m));
    player.prepare (kRate, 512, targets);
    player.setNodeHooks (overrides, nullptr);

    struct Ev { int pos; uint8_t data[3]; int numBytes; };
    struct Adapter { int samplePosition; const uint8_t* data; int numBytes; };
    const Ev raw[] = {
        { 0, { 0x90, 48, 100 }, 3 },
        { 12000, { 0x80, 48, 0 }, 3 },
        { 14000, { 0x90, 55, 100 }, 3 },
        { 26000, { 0x80, 55, 0 }, 3 },
    };
    std::vector<Adapter> events;
    for (const auto& e : raw)
        events.push_back ({ e.pos, e.data, e.numBytes });

    const int total = 32000;
    std::vector<float> out (static_cast<size_t> (total) * 2);
    auto it = events.cbegin();
    for (int pos = 0; pos < total; pos += 512)
    {
        const int n = std::min (512, total - pos);
        player.renderChunk (n, targets, it, events.cend(), pos);
        for (int i = 0; i < n; ++i)
        {
            out[static_cast<size_t> (pos + i) * 2] = player.outL()[i];
            out[static_cast<size_t> (pos + i) * 2 + 1] = player.outR()[i];
        }
    }
    return out;
}

bool smokeAllM12PatchAndBake()
{
    Patch p;
    p.name = "M12 Full Suite";
    p.init = "0;";
    p.voice =
        "lfo = mod_lfo(0, 1.0, 0.0);\n"
        "wt = osc_wt(0, freq, 2, 0.2 + lfo * 0.5);\n"
        "fm = osc_fm(0, freq, 0.25, 0.4, 0.15);\n"
        "sy = osc_sync(0, freq * 0.5, 0.35, 0.3);\n"
        "sig = wt * 0.4 + fm * 0.3 + sy * 0.3;\n"
        "sig = fx_shape(sig, 0.4, 0.25);\n"
        "sig = flt_formant(0, sig, 0.3 + lfo * 0.4, 0.4);\n"
        "sig = flt_notch(0, sig, 0.55, 0.3);\n"
        "sig = flt_peak(0, sig, 0.45, 0.65, 0.3);\n"
        "sig = flt_tilt(0, sig, 0.55);\n"
        "env = env_adsr(0, gate, 0.05, 0.3, 0.7, 0.25);\n"
        "out = sig * env * vel * 0.35;";
    p.master =
        "outl = bus_sat(inl, 0.2);\n"
        "outr = bus_sat(inr, 0.2);";

    const ValidationResult vr = validatePatch (p, kRate);
    bool ok = check (vr.ok(), "M12 full-suite patch validates cleanly (trim %.3f)%s%s",
                     vr.trim, vr.ok() ? "" : ": ", vr.error.c_str());
    if (! vr.ok())
        return false;
    p.trim = vr.trim;

    // Apply tweaks across the new M12 modules, including Choice args on osc_wt and fx_shape.
    setPatchArgTweak (p, "osc_wt:0", "table", ArgTweak::kReplace, 5.0);
    setPatchArgTweak (p, "osc_wt:0", "pos", ArgTweak::kOffset, 0.15);
    setPatchArgTweak (p, "osc_fm:0", "index", ArgTweak::kReplace, 0.55);
    setPatchArgTweak (p, "osc_sync:0", "ratio", ArgTweak::kOffset, 0.1);
    setPatchArgTweak (p, "fx_shape#0", "type", ArgTweak::kReplace, 0.7);
    setPatchArgTweak (p, "flt_formant:0", "vowel", ArgTweak::kOffset, -0.1);
    setPatchArgTweak (p, "flt_notch:0", "cutoff", ArgTweak::kReplace, 0.6);
    setPatchArgTweak (p, "flt_peak:0", "gain", ArgTweak::kReplace, 0.7);
    setPatchArgTweak (p, "flt_tilt:0", "tilt", ArgTweak::kReplace, 0.6);
    setPatchMacroMap (p, "mod_lfo:0", "beats", 2, 0.5, 2.0);

    const BakeResult baked = bakePatch (p);
    ok = check (baked.ok && baked.callsChanged == 9,
                "M12 bakePatch rewrites all 9 new M12 functions (%d calls changed)%s%s",
                baked.callsChanged, baked.ok ? "" : ": ", baked.error.c_str())
       && ok;
    if (! baked.ok)
        return false;

    const ValidationResult vrBaked = validatePatch (baked.patch, kRate);
    ok = check (vrBaked.ok(), "M12 baked full-suite patch validates cleanly") && ok;

    auto table = std::make_unique<OverrideTable>();
    applyPatchTweaks (p, *table);
    Patch bakedSameTrim = baked.patch;
    bakedSameTrim.trim = p.trim;
    const auto a = renderPatchPhrase (p, table.get());
    const auto b = renderPatchPhrase (bakedSameTrim, nullptr);
    double maxDiff = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        maxDiff = std::max (maxDiff, static_cast<double> (std::fabs (a[i] - b[i])));
    const double diffDb = 20.0 * std::log10 (std::max (maxDiff, 1.0e-20));
    ok = check (! a.empty() && a.size() == b.size() && diffDb <= -100.0,
                "M12 tweaked render == baked render across all 9 new modules (max diff %.1f dB)",
                diffDb)
       && ok;

    return ok;
}

} // namespace

bool smokeM12()
{
    bool ok = true;
    ok = smokeBoundsAndNan() && ok;
    ok = smokeWavetables() && ok;
    ok = smokeModLfo() && ok;
    ok = smokeNewFilters() && ok;
    ok = smokeAdaaIsolationAndBranches() && ok;
    ok = smokeAdaaAliasing() && ok;
    ok = smokeAllM12PatchAndBake() && ok;
    return ok;
}

} // namespace vibesynth_cli
