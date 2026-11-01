// M12 Tier 2 smoketests (SPEC §5.3 Tier 2):
//   - bounds/NaN at extreme and garbage arguments (and tempo) for fx_phaser,
//     fx_fshift, bus_delay, bus_width
//   - fx_phaser: depth 0 is dry; a notch where the 8 stages sum to 540 deg
//   - fx_fshift: a 1 kHz sine moves up / down by the shift, other sideband
//     suppressed; 0.5 keeps the level (allpass)
//   - bus_delay: ping-pong timing, sub kept out of the echoes, mix 0 dry,
//     full feedback bounded, a tempo change glides (no click)
//   - bus_width: 0.5 and mono input unchanged bit for bit, 0 = mono above
//     150 Hz, 1 doubles the high side
//   - bypass through the hook layer (PassArg / ForceArg)
//   - a patch using all four: validate, tweak, bake, render equivalence

#include "SmokeM12b.h"

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
constexpr double kVoiceRate = 96000.0;
constexpr double kPi = 3.14159265358979323846;

bool finiteF (float v) noexcept
{
    uint32_t b = 0;
    std::memcpy (&b, &v, sizeof (v));
    return ((b >> 23) & 0xFFu) != 0xFFu;
}

double rms (const std::vector<double>& x, size_t from)
{
    double s = 0.0;
    for (size_t i = from; i < x.size(); ++i)
        s += x[i] * x[i];
    return x.size() > from ? std::sqrt (s / static_cast<double> (x.size() - from)) : 0.0;
}

// Power of `x` at `hz` (single-bin DFT with a Hann window).
double toneDb (const std::vector<double>& x, size_t from, double hz, double sr)
{
    const size_t n = x.size() - from;
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * static_cast<double> (i) / static_cast<double> (n));
        const double ph = 2.0 * kPi * hz * static_cast<double> (i) / sr;
        re += w * x[from + i] * std::cos (ph);
        im += w * x[from + i] * std::sin (ph);
    }
    const double mag = std::sqrt (re * re + im * im) / (0.25 * static_cast<double> (n));
    return 20.0 * std::log10 (std::max (mag, 1.0e-12));
}

// ---------------------------------------------------------------------------
bool smokeBounds()
{
    const char* voiceCases[] = {
        "nan = sqrt(-1); big = pow(10, 30); out = fx_phaser(0, nan, -big, big, nan) + fx_phaser(1, big, big, -big, big) + fx_phaser(9, noise(), nan, 1, 1);",
        "nan = sqrt(-1); big = pow(10, 30); out = fx_fshift(0, nan, -big) + fx_fshift(0, big, big) + fx_fshift(7, noise(), nan);",
    };
    // A huge input passes the dry path as it is (like bus_reverb; the
    // output stage limits it), so those cases check only finiteness.
    struct MasterCase { const char* code; bool bounded; };
    const MasterCase masterCases[] = {
        { "nan = sqrt(-1); big = pow(10, 30); outl = bus_delay(inl * big, nan, nan, big, -big); outr = bus_delay_r();", false },
        { "nan = sqrt(-1); big = pow(10, 30); outl = bus_delay(inl, inr, -big, 1, 1); outr = bus_delay_r() + bus_delay(inl, inr, big, nan, nan);", true },
        { "nan = sqrt(-1); big = pow(10, 30); outl = bus_delay(inl * big, inr, 0.0625, 1, 1); outr = bus_delay_r();", true },
        { "nan = sqrt(-1); big = pow(10, 30); outl = bus_width(inl * big, nan, big); outr = bus_width_r();", false },
        { "nan = sqrt(-1); big = pow(10, 30); outl = bus_width(inl, -inr, -big) + bus_width(inl, inr, nan); outr = bus_width_r() + bus_width(nan, inr, 1);", true },
    };
    double macros[MacroSmoother::kNumMacros] = { 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5 };
    int passed = 0, total = 0;
    for (const char* code : voiceCases)
    {
        ++total;
        const PatchStrings ps = { "0;", code, "" };
        auto v = std::make_unique<EelVoice> (ps, kVoiceRate, 5u);
        if (! v->isValid())
            continue;
        Transport t;
        v->noteOn (440.0, 69.0, 0.9, t);
        bool ok = true;
        for (int i = 0; i < 8192; ++i)
        {
            const float s = v->renderSample (i < 4096 ? 440.0 : 1.0e9, 1.0, 69.0, 0.9, macros, t);
            ok = ok && finiteF (s) && std::fabs (s) < 100.0f;
        }
        passed += ok ? 1 : 0;
    }
    // Tempo extremes too: NaN, 0, 1e9 bpm.
    const double bpms[] = { 120.0, std::sqrt (-1.0), 0.0, 1.0e9, 20.0 };
    for (const MasterCase& mc : masterCases)
    {
        ++total;
        const PatchStrings ps = { "0;", "out = 0;", mc.code };
        auto m = std::make_unique<EelMaster> (ps, kRate);
        if (! m->isValid())
            continue;
        bool ok = true;
        for (int i = 0; i < 20000; ++i)
        {
            Transport t;
            t.bpm = bpms[(i / 4000) % 5];
            const float in = static_cast<float> (0.8 * std::sin (i * 0.05));
            float l = 0.0f, r = 0.0f;
            m->renderSample (in, -in, l, r, macros, t);
            ok = ok && finiteF (l) && finiteF (r) && (! mc.bounded || (std::fabs (l) < 100.0f && std::fabs (r) < 100.0f));
        }
        passed += ok ? 1 : 0;
        if (! ok)
            std::printf ("       failed: %s\n", mc.code);
    }
    return check (passed == total,
                  "M12 Tier 2 bounds/NaN: %d/%d cases (fx_phaser, fx_fshift, bus_delay, bus_width at +/-1e30, NaN, "
                  "absurd slots and tempo NaN/0/1e9) finite and bounded",
                  passed, total);
}

// ---------------------------------------------------------------------------
bool smokePhaser()
{
    bool ok = true;
    FilterRate fr;
    fr.init (kVoiceRate);

    // depth 0: dry, bit for bit, while the chain runs.
    PhaserState ph;
    bool exact = true;
    for (int i = 0; i < 4000; ++i)
    {
        const float x = static_cast<float> (0.7 * std::sin (i * 0.01) + 0.2 * std::sin (i * 0.37));
        exact = exact && ph.process (x, 0.3 + 0.2 * std::sin (i * 0.001), 0.0, 0.7, fr) == x;
    }
    ok = check (exact, "M12 fx_phaser depth 0 is the input bit for bit (feedback 0.7, moving pos)") && ok;

    // Notch: 8 equal first-order allpasses reach -540 deg where each gives
    // -67.5 deg, i.e. at f = fc tan(33.75 deg); depth 1 (equal mix) cancels it.
    const double pos = 0.5;
    const double G = fr.ladderG (PhaserState::kLoNorm + PhaserState::kSpanNorm * pos);
    const double g = G / (1.0 - G);
    const double fc = std::atan (g) * kVoiceRate / kPi;
    const double fNotch = fc * std::tan (33.75 * kPi / 180.0);
    const double fPeak = fc * std::tan (22.5 * kPi / 180.0);  // -360 deg: in phase, +6 dB
    auto level = [&] (double hz, double fbArg) {
        PhaserState p;
        std::vector<double> y;
        for (int i = 0; i < 48000; ++i)
            y.push_back (p.process (static_cast<float> (0.5 * std::sin (2.0 * kPi * hz * i / kVoiceRate)), pos, 1.0,
                                    fbArg, fr));
        return 20.0 * std::log10 (std::max (rms (y, 24000) / (0.5 / std::sqrt (2.0)), 1.0e-9));
    };
    const double notchDb = level (fNotch, 0.0);
    const double peakDb = level (fPeak, 0.0);
    const double fbPeakDb = level (fPeak, 1.0);
    ok = check (notchDb < -30.0 && std::fabs (peakDb) < 0.5 && fbPeakDb > peakDb + 3.0,
                "M12 fx_phaser at pos 0.5 (break %.0f Hz): notch at %.0f Hz %.1f dB (< -30), in-phase %.0f Hz %.2f dB "
                "(0 +-0.5), feedback 1 lifts it to %.1f dB",
                fc, fNotch, notchDb, fPeak, peakDb, fbPeakDb)
       && ok;
    return ok;
}

// ---------------------------------------------------------------------------
bool smokeFshift()
{
    bool ok = true;
    // The shift that gives exactly +100 Hz: |s| = log2(100 * 1023 / 2000 + 1) / 10.
    const double s100 = std::log2 (100.0 * 1023.0 / 2000.0 + 1.0) / 10.0;
    const double up = 0.5 + 0.5 * s100, down = 0.5 - 0.5 * s100;
    const double mapped = FshiftState::shiftHz (up);
    auto run = [&] (double shift) {
        FshiftState fs;
        std::vector<double> y;
        for (int i = 0; i < 96000; ++i)
            y.push_back (fs.process (static_cast<float> (0.5 * std::sin (2.0 * kPi * 1000.0 * i / kVoiceRate)), shift,
                                     kVoiceRate));
        return y;
    };
    const auto yu = run (up);
    const auto yd = run (down);
    const auto y0 = run (0.5);
    const double u1100 = toneDb (yu, 48000, 1100.0, kVoiceRate), u900 = toneDb (yu, 48000, 900.0, kVoiceRate);
    const double d900 = toneDb (yd, 48000, 900.0, kVoiceRate), d1100 = toneDb (yd, 48000, 1100.0, kVoiceRate);
    const double flat = 20.0 * std::log10 (rms (y0, 48000) / (0.5 / std::sqrt (2.0)));
    ok = check (std::fabs (mapped - 100.0) < 1e-6 && u1100 > -7.0 && u1100 - u900 > 35.0 && d900 > -7.0
                    && d900 - d1100 > 35.0 && std::fabs (flat) < 0.1,
                "M12 fx_fshift 1 kHz sine: +100 Hz -> 1100 Hz at %.1f dB, 900 Hz sideband %.1f dB below; -100 Hz -> "
                "900 Hz, 1100 Hz %.1f dB below (need 35); shift 0.5 keeps the level (%.2f dB)",
                u1100, u1100 - u900, d900 - d1100, flat)
       && ok;
    return ok;
}

// ---------------------------------------------------------------------------
struct DelayRun
{
    std::vector<double> l, r;
};

DelayRun runDelay (int samples, double beats, double fb, double mix, const float* inL, const float* inR,
                   double bpmA = 120.0, double bpmB = 120.0, int switchAt = -1)
{
    const int frames = DelayBusState::framesFor (kRate);
    std::vector<float> storage ((size_t) frames * 2u);
    DelayBusState d;
    d.init (kRate, storage.data(), frames);
    DelayRun out;
    for (int i = 0; i < samples; ++i)
    {
        float l = 0.0f, r = 0.0f;
        d.process (inL[i], inR[i], beats, fb, mix, (switchAt >= 0 && i >= switchAt) ? bpmB : bpmA, l, r);
        out.l.push_back (l);
        out.r.push_back (r);
    }
    return out;
}

int peakIndex (const std::vector<double>& x, int from, int to)
{
    int best = from;
    for (int i = from; i < to && i < (int) x.size(); ++i)
        if (std::fabs (x[(size_t) i]) > std::fabs (x[(size_t) best]))
            best = i;
    return best;
}

bool smokeDelay()
{
    bool ok = true;
    const int n = 60000;
    std::vector<float> click ((size_t) n, 0.0f), zero ((size_t) n, 0.0f);
    click[100] = 1.0f;
    // 0.5 beat at 120 bpm = 12000 samples: L echo at +12000, R at +24000, L at +36000.
    const DelayRun d = runDelay (n, 0.5, 0.6, 1.0, click.data(), click.data());
    const int pL1 = peakIndex (d.l, 1000, 20000), pR1 = peakIndex (d.r, 1000, 30000);
    const int pL2 = peakIndex (d.l, 30000, 42000);
    double rBefore = 0.0;
    for (int i = 0; i < 18000; ++i)
        rBefore = std::max (rBefore, std::fabs (d.r[(size_t) i]));
    const double a1 = std::fabs (d.l[(size_t) pL1]), a2 = std::fabs (d.r[(size_t) pR1]),
                 a3 = std::fabs (d.l[(size_t) pL2]);
    ok = check (std::abs (pL1 - 12100) < 40 && std::abs (pR1 - 24100) < 80 && std::abs (pL2 - 36100) < 120
                    && rBefore < 1e-6 && a2 < a1 && a3 < a2,
                "M12 bus_delay ping-pong: echoes at L %d, R %d, L %d (expected 12100, 24100, 36100), nothing on R "
                "before its first echo, each quieter (%.3f, %.3f, %.3f)",
                pL1, pR1, pL2, a1, a2, a3)
       && ok;

    // The sub stays out of the echoes: a 40 Hz sine, fully wet.
    std::vector<float> sub ((size_t) n), mid ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        sub[(size_t) i] = static_cast<float> (0.5 * std::sin (2.0 * kPi * 40.0 * i / kRate));
        mid[(size_t) i] = static_cast<float> (0.5 * std::sin (2.0 * kPi * 1000.0 * i / kRate));
    }
    const DelayRun ds = runDelay (n, 0.25, 0.0, 1.0, sub.data(), sub.data());
    const DelayRun dm = runDelay (n, 0.25, 0.0, 1.0, mid.data(), mid.data());
    const double subDb = 20.0 * std::log10 (std::max (rms (ds.l, 20000) / (0.5 / std::sqrt (2.0)), 1e-9));
    const double midDb = 20.0 * std::log10 (std::max (rms (dm.l, 20000) / (0.5 / std::sqrt (2.0)), 1e-9));
    ok = check (subDb < -20.0 && midDb > -1.5,
                "M12 bus_delay keeps the sub dry: a 40 Hz echo is %.1f dB (< -20), a 1 kHz echo %.1f dB", subDb, midDb)
       && ok;

    // mix 0: dry bit for bit (the ForceArg bypass).
    std::vector<float> noiseL ((size_t) n), noiseR ((size_t) n);
    uint32_t seed = 12345u;
    for (int i = 0; i < n; ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        noiseL[(size_t) i] = static_cast<float> ((seed >> 8) * (1.0 / 16777216.0) - 0.5);
        seed = seed * 1664525u + 1013904223u;
        noiseR[(size_t) i] = static_cast<float> ((seed >> 8) * (1.0 / 16777216.0) - 0.5);
    }
    const DelayRun dry = runDelay (n, 0.75, 0.8, 0.0, noiseL.data(), noiseR.data());
    bool exact = true;
    for (int i = 0; i < n; ++i)
        exact = exact && (float) dry.l[(size_t) i] == noiseL[(size_t) i] && (float) dry.r[(size_t) i] == noiseR[(size_t) i];
    // Full feedback on loud noise for 5 s stays bounded.
    const int nLong = 240000;
    std::vector<float> loud ((size_t) nLong);
    for (int i = 0; i < nLong; ++i)
        loud[(size_t) i] = static_cast<float> (noiseL[(size_t) (i % n)] * 2.0);
    const DelayRun fbRun = runDelay (nLong, 0.0625, 1.0, 1.0, loud.data(), loud.data());
    double fbPeak = 0.0;
    bool fbFinite = true;
    for (int i = 0; i < nLong; ++i)
    {
        fbPeak = std::max ({ fbPeak, std::fabs (fbRun.l[(size_t) i]), std::fabs (fbRun.r[(size_t) i]) });
        fbFinite = fbFinite && finiteF ((float) fbRun.l[(size_t) i]);
    }
    ok = check (exact && fbFinite && fbPeak < 4.0,
                "M12 bus_delay mix 0 is dry bit for bit; feedback 1 on loud noise for 5 s at the shortest time stays "
                "bounded (peak %.2f)",
                fbPeak)
       && ok;

    // Tempo change 120 -> 150 bpm mid-note: the delay time glides. The
    // largest sample-to-sample step of the wet signal stays near a steady
    // sine's (a jump in read position would click).
    std::vector<float> tone ((size_t) n);
    for (int i = 0; i < n; ++i)
        tone[(size_t) i] = static_cast<float> (0.5 * std::sin (2.0 * kPi * 300.0 * i / kRate));
    const DelayRun tc = runDelay (n, 0.75, 0.3, 1.0, tone.data(), tone.data(), 120.0, 150.0, 30000);
    double stepBefore = 0.0, stepAfter = 0.0;
    for (int i = 20001; i < 30000; ++i)
        stepBefore = std::max (stepBefore, std::fabs (tc.l[(size_t) i] - tc.l[(size_t) i - 1]));
    for (int i = 30000; i < n; ++i)
        stepAfter = std::max (stepAfter, std::fabs (tc.l[(size_t) i] - tc.l[(size_t) i - 1]));
    ok = check (stepBefore > 0.0 && stepAfter < stepBefore * 1.3,
                "M12 bus_delay tempo change 120 -> 150 bpm glides: largest wet step %.4f vs %.4f steady (< 1.3x)",
                stepAfter, stepBefore)
       && ok;
    return ok;
}

// ---------------------------------------------------------------------------
bool smokeWidth()
{
    bool ok = true;
    const int n = 48000;
    auto run = [&] (double amount, double hzSide, double hzMid, bool monoIn, double& sideRatio, bool& exact) {
        WidthBusState w;
        w.init (kRate);
        double sIn = 0.0, sOut = 0.0;
        exact = true;
        for (int i = 0; i < n; ++i)
        {
            const double m = 0.3 * std::sin (2.0 * kPi * hzMid * i / kRate);
            const double s = monoIn ? 0.0 : 0.3 * std::sin (2.0 * kPi * hzSide * i / kRate);
            const float l = static_cast<float> (m + s), r = static_cast<float> (m - s);
            float ol = 0.0f, orr = 0.0f;
            w.process (l, r, amount, ol, orr);
            exact = exact && ol == l && orr == r;
            if (i >= n / 2)
            {
                const double si = 0.5 * ((double) l - (double) r), so = 0.5 * ((double) ol - (double) orr);
                sIn += si * si;
                sOut += so * so;
            }
        }
        sideRatio = sIn > 0.0 ? std::sqrt (sOut / sIn) : 0.0;
    };
    double r05 = 0.0, rMono = 0.0, r0 = 0.0, r1 = 0.0, rLow = 0.0;
    bool e05 = false, eMono = false, dummy = false;
    run (0.5, 2000.0, 300.0, false, r05, e05);
    run (1.0, 2000.0, 50.0, true, rMono, eMono);
    run (0.0, 2000.0, 300.0, false, r0, dummy);
    run (1.0, 2000.0, 300.0, false, r1, dummy);
    run (1.0, 40.0, 300.0, false, rLow, dummy);
    ok = check (e05 && eMono && r0 < 0.02 && std::fabs (r1 - 2.0) < 0.05 && rLow < 1.15,
                "M12 bus_width: 0.5 unchanged bit for bit; mono input unchanged bit for bit at 1; 2 kHz side x%.3f at 0 "
                "(mono), x%.3f at 1 (2); a 40 Hz side only x%.3f at 1 (low end untouched)",
                r0, r1, rLow)
       && ok;
    return ok;
}

// ---------------------------------------------------------------------------
// Bypass through the hook layer: PassArg for the voice effects, ForceArg
// (mix 0 / amount 0.5) for the bus functions, so both channels are dry.
bool smokeBypass()
{
    bool ok = true;
    double macros[8] = { 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5 };
    {
        const PatchStrings ps { "", "out = 0;",
                                "d = bus_delay(inl, inr, 0.25, 0.7, 0.6); dr = bus_delay_r();\n"
                                "outl = bus_width(d, dr, 1); outr = bus_width_r();" };
        OverrideTable t;
        t.setBypass (RackKind::Master, ModuleId::BusDelay, 0, true);
        t.setBypass (RackKind::Master, ModuleId::BusWidth, 0, true);
        EelMaster m (ps, kRate);
        m.voiceRack().overrides = &t;
        m.rackContext().hooked = true;
        bool exact = m.isValid();
        for (int i = 0; exact && i < 30000; ++i)
        {
            const float inL = (float) (0.6 * std::sin (i * 0.031)), inR = (float) (0.5 * std::sin (i * 0.047 + 1.0));
            float l = 0.0f, r = 0.0f;
            m.renderSample (inL, inR, l, r, macros);
            exact = l == inL && r == inR;
        }
        ok = check (exact, "M12 bypass (bus_delay mix 0, bus_width 0.5): both channels dry, bit for bit") && ok;
    }
    {
        const PatchStrings ps { "", "x = sin(srate * 0 + t * 0.05); t += 1; out = fx_phaser(0, x, 0.4, 1, 0.8) + fx_fshift(0, x, 0.8) - 2 * x;",
                                "" };
        OverrideTable t;
        t.setBypass (RackKind::Voice, ModuleId::FxPhaser, 0, true);
        t.setBypass (RackKind::Voice, ModuleId::FxFshift, 0, true);
        EelVoice v (ps, kVoiceRate, 3u);
        v.rack().overrides = &t;
        v.rackContext().hooked = true;
        bool zero = v.isValid();
        v.noteOn (220.0, 57.0, 1.0);
        for (int i = 0; zero && i < 4000; ++i)
            zero = v.renderSample (220.0, 1.0, 57.0, 1.0, macros) == 0.0f;
        ok = check (zero, "M12 bypass (fx_phaser, fx_fshift): input passes through") && ok;
    }
    return ok;
}

// ---------------------------------------------------------------------------
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

    const int total = 48000;
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

bool smokeTier2PatchAndBake()
{
    Patch p;
    p.name = "M12 Tier 2 Suite";
    p.init = "0;";
    p.voice =
        "lfo = mod_lfo(0, 2.0, 0.0);\n"
        "s = osc_saw(0, freq) * 0.5 + osc_saw(1, freq * 1.005) * 0.5;\n"
        "s = fx_phaser(0, s, lfo, 0.8, 0.5);\n"
        "s = s * 0.7 + fx_fshift(0, s, 0.56) * 0.3;\n"
        "env = env_adsr(0, gate, 0.02, 0.3, 0.7, 0.25);\n"
        "out = s * env * vel * 0.35;";
    p.master =
        "d = bus_delay(inl, inr, 0.75, 0.4, 0.25);\n"
        "dr = bus_delay_r();\n"
        "outl = bus_width(d, dr, 0.7);\n"
        "outr = bus_width_r();";

    const ValidationResult vr = validatePatch (p, kRate);
    bool ok = check (vr.ok(), "M12 Tier 2 suite patch validates cleanly (trim %.3f)%s%s", vr.trim,
                     vr.ok() ? "" : ": ", vr.error.c_str());
    if (! vr.ok())
        return false;
    p.trim = vr.trim;

    // Node ids: the bus functions have no slot.
    NodeRef nd, nw, np;
    const bool ids = parseNodeId ("master/bus_delay", nd) && nd.module == ModuleId::BusDelay
                     && nodeIdString (nd) == "master/bus_delay" && parseNodeId ("master/bus_width", nw)
                     && nw.module == ModuleId::BusWidth && nodeIdString (nw) == "master/bus_width"
                     && parseNodeId ("fx_phaser:1", np) && np.slot == 1 && nodeIdString (np) == "fx_phaser:1";
    ok = check (ids, "M12 Tier 2 node ids: master/bus_delay, master/bus_width (no slot), fx_phaser:1 round-trip") && ok;

    setPatchArgTweak (p, "fx_phaser:0", "depth", ArgTweak::kReplace, 0.6);
    setPatchArgTweak (p, "fx_fshift:0", "shift", ArgTweak::kOffset, 0.05);
    setPatchArgTweak (p, "master/bus_delay", "feedback", ArgTweak::kReplace, 0.55);
    setPatchMacroMap (p, "master/bus_width", "amount", 3, 0.4, 0.9);

    const BakeResult baked = bakePatch (p);
    ok = check (baked.ok && baked.callsChanged == 4, "M12 Tier 2 bakePatch rewrites all 4 functions (%d calls changed)%s%s",
                baked.callsChanged, baked.ok ? "" : ": ", baked.error.c_str())
       && ok;
    if (! baked.ok)
        return false;

    auto table = std::make_unique<OverrideTable>();
    applyPatchTweaks (p, *table);
    Patch bakedSameTrim = baked.patch;
    bakedSameTrim.trim = p.trim;
    const auto a = renderPatchPhrase (p, table.get());
    const auto b = renderPatchPhrase (bakedSameTrim, nullptr);
    double maxDiff = 0.0, side = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        maxDiff = std::max (maxDiff, static_cast<double> (std::fabs (a[i] - b[i])));
    for (size_t i = 0; i + 1 < a.size(); i += 2)
        side = std::max (side, static_cast<double> (std::fabs (a[i] - a[i + 1])));
    const double diffDb = 20.0 * std::log10 (std::max (maxDiff, 1.0e-20));
    ok = check (! a.empty() && a.size() == b.size() && diffDb <= -100.0 && side > 0.01,
                "M12 Tier 2 tweaked render == baked render (max diff %.1f dB); the master is stereo (max |L-R| %.3f)",
                diffDb, side)
       && ok;
    return ok;
}

} // namespace

bool smokeM12b()
{
    bool ok = true;
    ok = smokeBounds() && ok;
    ok = smokePhaser() && ok;
    ok = smokeFshift() && ok;
    ok = smokeDelay() && ok;
    ok = smokeWidth() && ok;
    ok = smokeBypass() && ok;
    ok = smokeTier2PatchAndBake() && ok;
    return ok;
}

} // namespace vibesynth_cli
