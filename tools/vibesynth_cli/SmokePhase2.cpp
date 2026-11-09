// Phase 2 sound-tool smoketests:
//   - bus_reverb: mix 0 is the dry input bit for bit; at mix 1 a held saw's
//     wet level is near the input's (averaged over 12 pitches; the spread of
//     single pitches is reported); decay time against size; L/R correlation
//   - fx_comb_ff: sine-tone magnitudes against (1 + ff Z) / (1 - fb Z) at the
//     voice and the master rate; argument order through the VM; a moving or
//     jumping hz adds no energy above 8 kHz (no clicks, no zipper)
//   - bus_mb: neutral settings sum flat 20 Hz-20 kHz; each band's static
//     curve against the formula with sine tones at 55, 880 and 7040 Hz from
//     -80 to 0 dBFS; channels compressed separately; mix; control-rate reads
//   - bounds and NaN for all three. EEL2 can't make NaN or Inf in patch code
//     (its sqrt takes |x|, and assignment flushes them to 0), so they go
//     straight into the functions, the states and mem[].

#include "SmokePhase2.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "eel2/ns-eel.h"
#include "CliCheck.h"
#include "engine/Lexicon.h"
#include "engine/MasterRack.h"
#include "patch/PatchEngine.h"

namespace vibesynth {
// Defined in LexiconFn6.cpp / LexiconFn8.cpp; called directly so NaN reaches them.
EEL_F NSEEL_CGEN_CALL fnBusReverb (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnFxCombFf (void*, INT_PTR, EEL_F**);
EEL_F NSEEL_CGEN_CALL fnBusMb (void*, INT_PTR, EEL_F**);
} // namespace vibesynth

namespace vibesynth_cli {

namespace {

using namespace vibesynth;

constexpr double kRate = 48000.0;
constexpr double kVoiceRate = 96000.0;
constexpr double kPi = 3.14159265358979323846;

const double kBad[] = { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                        -std::numeric_limits<double>::infinity(), 1.0e30, -1.0e30 };

bool finiteF (float v) noexcept
{
    uint32_t b = 0;
    std::memcpy (&b, &v, sizeof (v));
    return ((b >> 23) & 0xFFu) != 0xFFu;
}

bool sameBitsF (float a, float b) noexcept
{
    return std::memcmp (&a, &b, sizeof (float)) == 0;
}

double db (double x)
{
    return 20.0 * std::log10 (std::max (x, 1.0e-30));
}

// Amplitude of the `hz` component of y[from .. from + n) (Hann window).
double toneAmp (const std::vector<double>& y, size_t from, size_t n, double hz, double sr)
{
    double re = 0.0, im = 0.0, wsum = 0.0;
    const double step = 2.0 * kPi * hz / sr;
    const double cr = std::cos (step), sr1 = std::sin (step);
    double c = 1.0, s = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * static_cast<double> (i) / static_cast<double> (n - 1));
        re += w * y[from + i] * c;
        im += w * y[from + i] * s;
        wsum += w;
        const double nc = c * cr - s * sr1;
        s = s * cr + c * sr1;
        c = nc;
    }
    return 2.0 * std::sqrt (re * re + im * im) / wsum;
}

// A sine oscillator by rotation (cheap, exact enough for these tests).
struct Sine
{
    double c = 0.0, s = -1.0, cr = 1.0, sr = 0.0;
    Sine (double hz, double rate) : cr (std::cos (2.0 * kPi * hz / rate)), sr (std::sin (2.0 * kPi * hz / rate))
    {
        c = 1.0;
        s = 0.0;
    }
    double next()
    {
        const double out = s;
        const double nc = c * cr - s * sr;
        s = s * cr + c * sr;
        c = nc;
        return out;
    }
};

// RBJ biquad for test filters.
struct TestBiquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    static TestBiquad make (int kind, double hz, double q, double rate)  // 0 LP, 1 HP, 2 BP (0 dB peak)
    {
        const double w0 = 2.0 * kPi * hz / rate, cw = std::cos (w0), al = std::sin (w0) / (2.0 * q), a0 = 1.0 + al;
        TestBiquad f;
        if (kind == 0) { f.b0 = (1 - cw) / 2 / a0; f.b1 = (1 - cw) / a0; f.b2 = f.b0; }
        else if (kind == 1) { f.b0 = (1 + cw) / 2 / a0; f.b1 = -(1 + cw) / a0; f.b2 = f.b0; }
        else { f.b0 = al / a0; f.b1 = 0.0; f.b2 = -al / a0; }
        f.a1 = -2.0 * cw / a0;
        f.a2 = (1.0 - al) / a0;
        return f;
    }
    double process (double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

std::complex<double> biquadResponse (const MbCoef& q, double hz, double rate)
{
    const std::complex<double> z1 = std::polar (1.0, -2.0 * kPi * hz / rate);
    return (q.b0 + q.b1 * z1 + q.b2 * z1 * z1) / (1.0 + q.a1 * z1 + q.a2 * z1 * z1);
}

std::unique_ptr<EelMaster> makeMaster (const char* code, double rate = kRate)
{
    const PatchStrings ps = { "0;", "out = 0;", code };
    return std::make_unique<EelMaster> (ps, rate);
}

bool poke (void* vm, int index, double value)
{
    int valid = 0;
    EEL_F* ram = NSEEL_VM_getramptr_noalloc (static_cast<NSEEL_VMCTX> (vm), static_cast<unsigned int> (index), &valid);
    if (ram == nullptr || valid < 1)
        return false;
    *ram = value;
    return std::memcmp (ram, &value, sizeof (value)) == 0;
}

// ===========================================================================
// bus_reverb

bool smokeReverb()
{
    bool ok = true;
    double macros[MacroSmoother::kNumMacros] = { 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5 };

    // Mix 0: the dry input, bit for bit, on both channels, whatever the size
    // and whatever the reverb holds from before.
    {
        bool exact = true;
        for (const char* code : { "outl = bus_reverb(inl, inr, 0, 0); outr = bus_reverb_r();",
                                  "outl = bus_reverb(inl, inr, 0.8, 0); outr = bus_reverb_r();",
                                  "outl = bus_reverb(inl, inr, 1, beat > 0.5 ? 0 : 1); outr = bus_reverb_r();" })
        {
            auto m = makeMaster (code);
            exact = exact && m->isValid();
            uint32_t x = 0x1234567u;
            Transport t;
            for (int i = 0; i < 24000 && exact; ++i)
            {
                t.beat = i / 12000.0;
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                const float l = static_cast<float> ((x & 0xFFFF) / 65535.0 - 0.5);
                const float r = static_cast<float> (((x >> 16) & 0xFFFF) / 65535.0 - 0.5);
                float ol = 0.0f, orr = 0.0f;
                m->renderSample (l, r, ol, orr, macros, t);
                if (t.beat > 1.0)  // the switch to mix 0 at beat 0.5 fades the wet out first (about 46 ms)
                    exact = exact && sameBitsF (ol, l) && sameBitsF (orr, r);
            }
        }
        ok = check (exact, "bus_reverb mix 0 is the dry input bit for bit on both channels (sizes 0 and 0.8 from the "
                           "start; size 1 once a switch from mix 1 has faded)") && ok;
    }

    // Wet level at mix 1 on a held saw (0.1 peak): 12 pitches, 2 semitones
    // apart, from 55 Hz and from 220 Hz; RMS over 2-3 s against the saw's own.
    {
        const double sizes[] = { 0.2, 0.5, 0.8 };
        const double bases[] = { 55.0, 220.0 };
        double worstMean = 0.0, worstPitch = 0.0, corrSum = 0.0;
        int corrCount = 0;
        char detail[512] = {};
        for (double size : sizes)
            for (double base : bases)
            {
                double powSum = 0.0, lo = 1.0e9, hi = -1.0e9;
                for (int k = 0; k < 12; ++k)
                {
                    const double hz = base * std::pow (2.0, 2.0 * k / 12.0);
                    char code[256];
                    std::snprintf (code, sizeof (code),
                                   "x = osc_saw(0, %.6f) * 0.1; n >= 96000 ? acc += x * x; n += 1;"
                                   " outl = bus_reverb(x, x, %.2f, 1); outr = bus_reverb_r();", hz, size);
                    auto m = makeMaster (code);
                    if (! m->isValid())
                        return check (false, "bus_reverb level patch compiles: %s", m->compileError().c_str());
                    double wl = 0.0, wr = 0.0, lr = 0.0;
                    for (int i = 0; i < 144000; ++i)
                    {
                        float ol = 0.0f, orr = 0.0f;
                        m->renderSample (0.0f, 0.0f, ol, orr, macros);
                        if (i >= 96000)
                        {
                            wl += static_cast<double> (ol) * ol;
                            wr += static_cast<double> (orr) * orr;
                            lr += static_cast<double> (ol) * orr;
                        }
                    }
                    const double dry = *m->variable ("acc");
                    const double ratio = 0.5 * (wl + wr) / dry;
                    powSum += ratio;
                    lo = std::min (lo, 10.0 * std::log10 (ratio));
                    hi = std::max (hi, 10.0 * std::log10 (ratio));
                    if (size == 0.5)
                    {
                        corrSum += lr / std::sqrt (wl * wr);
                        ++corrCount;
                    }
                }
                const double meanDb = 10.0 * std::log10 (powSum / 12.0);
                worstMean = std::max (worstMean, std::fabs (meanDb));
                worstPitch = std::max ({ worstPitch, std::fabs (lo), std::fabs (hi) });
                const size_t used = std::strlen (detail);
                std::snprintf (detail + used, sizeof (detail) - used, "%s%.1f/%.0f Hz: mean %+.2f, pitches %+.1f..%+.1f",
                               used > 0 ? "; " : "", size, base, meanDb, lo, hi);
            }
        std::printf ("[INFO] bus_reverb wet minus dry at mix 1, held saw, dB (size/lowest pitch): %s\n", detail);
        std::printf ("[NOTE] bus_reverb held saw at mix 1: single pitches differ from the input by up to %.1f dB, power "
                     "means of 12 pitches by up to %.2f dB (a long reverb's response to one steady tone depends on where "
                     "its harmonics land)\n", worstPitch, worstMean);
        std::printf ("[INFO] bus_reverb wet L/R correlation, held saw at size 0.5, mean of 24 pitches: %+.3f\n",
                     corrSum / std::max (1, corrCount));
    }

    // The normalisation itself, on a smooth spectrum: noise low-passed at
    // 2 kHz, mono, wet RMS over 2-4 s against the input's.
    {
        double worst = 0.0;
        char detail[160] = {};
        for (double size : { 0.2, 0.5, 0.8 })
        {
            char code[160];
            std::snprintf (code, sizeof (code), "outl = bus_reverb(inl, inr, %.2f, 1); outr = bus_reverb_r();", size);
            auto m = makeMaster (code);
            TestBiquad lp1 = TestBiquad::make (0, 2000.0, 0.7071, kRate), lp2 = lp1;
            uint32_t x = 0x2468ACEu;
            double wet = 0.0, dry = 0.0;
            for (int i = 0; i < 192000; ++i)
            {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                const double in = lp2.process (lp1.process ((x & 0xFFFFFF) / 16777215.0 - 0.5)) * 0.2;
                float ol = 0.0f, orr = 0.0f;
                m->renderSample (static_cast<float> (in), static_cast<float> (in), ol, orr, macros);
                if (i >= 96000)
                {
                    wet += 0.5 * (static_cast<double> (ol) * ol + static_cast<double> (orr) * orr);
                    dry += in * in;
                }
            }
            const double d = 10.0 * std::log10 (wet / dry);
            worst = std::max (worst, std::fabs (d));
            const size_t used = std::strlen (detail);
            std::snprintf (detail + used, sizeof (detail) - used, "%s%.1f: %+.2f", used ? ", " : "", size, d);
        }
        ok = check (worst <= 1.0, "bus_reverb wet level at mix 1 on noise low-passed at 2 kHz: within 1 dB of the input "
                    "(size: dB %s)", detail) && ok;
    }

    // Decay time against size: impulse response, 500 Hz band (one octave),
    // Schroeder integration, -5 to -25 dB slope x 3.
    {
        char detail[512] = {};
        bool near = true;
        for (double size : { 0.0, 0.2, 0.5, 0.8, 1.0 })
        {
            char code[160];
            std::snprintf (code, sizeof (code), "outl = bus_reverb(inl, inr, %.2f, 1); outr = bus_reverb_r();", size);
            auto m = makeMaster (code);
            const double nominal = FdnReverbState::rt60For (size);
            const int n = static_cast<int> (kRate * (nominal * 1.6 + 0.3));
            std::vector<double> e (static_cast<size_t> (n));
            TestBiquad bl = TestBiquad::make (2, 500.0, 1.41, kRate), br = bl;
            for (int i = 0; i < n; ++i)
            {
                float ol = 0.0f, orr = 0.0f;
                const float in = i == 0 ? 1.0f : 0.0f;
                m->renderSample (in, in, ol, orr, macros);
                const double a = bl.process (ol), b = br.process (orr);
                e[static_cast<size_t> (i)] = a * a + b * b;
            }
            for (int i = n - 2; i >= 0; --i)
                e[static_cast<size_t> (i)] += e[static_cast<size_t> (i) + 1];
            const double total = e[0];
            int t5 = -1, t25 = -1;
            for (int i = 0; i < n; ++i)
            {
                const double d = 10.0 * std::log10 (e[static_cast<size_t> (i)] / total);
                if (t5 < 0 && d <= -5.0) t5 = i;
                if (t25 < 0 && d <= -25.0) { t25 = i; break; }
            }
            const double rt = t5 >= 0 && t25 > t5 ? 3.0 * (t25 - t5) / kRate : -1.0;
            near = near && rt > 0.0 && std::fabs (rt / nominal - 1.0) <= 0.25;
            const size_t used = std::strlen (detail);
            std::snprintf (detail + used, sizeof (detail) - used, "%ssize %.1f: %.2f s (nominal %.2f)", used > 0 ? ", " : "",
                           size, rt, nominal);
        }
        ok = check (near, "bus_reverb decay time follows size, 500 Hz band, within 25%% of 0.3 * 20^size: %s", detail) && ok;
    }

    // Moving size or mix doesn't click: a jump from size 0.2 to 0.9 (and mix
    // 0.3 to 0.6) mid-note adds no energy above 10 kHz on a 200 Hz sine.
    {
        auto m = makeMaster ("x = osc_sine(0, 200) * 0.5; jump = beat > 2;"
                             " outl = bus_reverb(x, x, jump ? 0.9 : 0.2, jump ? 0.6 : 0.3); outr = bus_reverb_r();");
        TestBiquad h1 = TestBiquad::make (1, 10000.0, 0.7071, kRate), h2 = h1;
        Transport t;
        double before = 0.0, after = 0.0, sig = 0.0;
        for (int i = 0; i < 96000; ++i)
        {
            t.beat = i / 24000.0;  // beat 2 at sample 48000
            float ol = 0.0f, orr = 0.0f;
            m->renderSample (0.0f, 0.0f, ol, orr, macros, t);
            const double h = h2.process (h1.process (ol));
            if (i >= 38000 && i < 47000) before = std::max (before, std::fabs (h));
            if (i >= 47990 && i < 49000) after = std::max (after, std::fabs (h));
            if (i >= 38000) sig = std::max (sig, std::fabs (static_cast<double> (ol)));
        }
        ok = check (after <= std::max (2.0 * before, 1.0e-4 * sig),
                    "bus_reverb size 0.2 -> 0.9 and mix 0.3 -> 0.6 in one sample: largest output above 10 kHz %.1f dB "
                    "below the signal peak (%.1f dB before the jump)", -db (after / sig), -db (before / sig)) && ok;
    }

    // Bounds and NaN. (a) Straight into the wrapper, each argument at each bad value.
    {
        auto m = makeMaster ("outl = bus_reverb(inl, inr, 0.5, 0.5); outr = bus_reverb_r();");
        int cases = 0;
        bool safe = m->isValid();
        for (int arg = 0; arg < 4 && safe; ++arg)
            for (double bad : kBad)
            {
                ++cases;
                m->resetRacks();  // a 1e30 input leaves a 1e30 tail for the next case
                for (int i = 0; i < 3000; ++i)
                {
                    EEL_F a[4] = { 0.5 * std::sin (i * 0.05), -0.4 * std::sin (i * 0.07), 0.5, 0.5 };
                    if (i >= 1000) a[arg] = bad;
                    EEL_F* ptrs[4] = { &a[0], &a[1], &a[2], &a[3] };
                    const double l = fnBusReverb (&m->rackContext(), 4, ptrs);
                    const float r = m->rack().reverbRight;
                    const bool hugeIn = arg < 2 && std::fabs (bad) > 1.0;  // a huge input passes the dry path as it is
                    safe = safe && finiteF (static_cast<float> (l)) && finiteF (r)
                           && (hugeIn || (std::fabs (l) < 100.0 && std::fabs (r) < 100.0f));
                }
                safe = safe && std::isfinite (fnBusReverb (&m->rackContext(), 0, nullptr) + 0.0);
            }
        // (b) The state itself, bad inputs and sizes, then normal input: finite and bounded again.
        FdnReverbState s;
        std::vector<float> store (static_cast<size_t> (FdnReverbState::storageFor (kRate)));
        s.init (kRate, store.data(), static_cast<int> (store.size()));
        for (double bad : kBad)
        {
            ++cases;
            for (int i = 0; i < 4000; ++i)
            {
                float wl = 0.0f, wr = 0.0f;
                const bool late = i >= 2000;
                if (i == 2000)
                    s.reset();  // after a 1e30 input the tail is legitimately huge
                s.process (late ? 0.3f : static_cast<float> (bad), late ? -0.3f : static_cast<float> (bad), late ? 0.7 : bad,
                           kRate, wl, wr);
                safe = safe && finiteF (wl) && finiteF (wr) && (! late || (std::fabs (wl) < 100.0f && std::fabs (wr) < 100.0f));
            }
        }
        // (c) Through the VM: extremes EEL2 can make.
        for (const char* code : { "big = pow(10, 30); outl = bus_reverb(inl, inr, big, -big); outr = bus_reverb_r();",
                                  "big = pow(10, 30); outl = bus_reverb(inl, inr, -big, big); outr = bus_reverb_r();",
                                  "outl = bus_reverb(inl, inr, rand(3) - 1, rand(3) - 1); outr = bus_reverb_r();" })
        {
            auto v = makeMaster (code);
            safe = safe && v->isValid();
            for (int i = 0; i < 20000 && safe; ++i)
            {
                float ol = 0.0f, orr = 0.0f;
                const float in = static_cast<float> (0.8 * std::sin (i * 0.05));
                v->renderSample (in, -in, ol, orr, macros);
                safe = safe && finiteF (ol) && finiteF (orr) && std::fabs (ol) < 100.0f && std::fabs (orr) < 100.0f;
            }
            ++cases;
        }
        ok = check (safe, "bus_reverb bounds/NaN: %d cases of NaN, Inf and +/-1e30 straight into the function and its "
                    "state, and VM extremes: finite, bounded unless the input itself is huge", cases) && ok;
    }
    return ok;
}

// ===========================================================================
// fx_comb_ff

double combFormulaDb (double f, double hz, double fb, double ff)
{
    const std::complex<double> z = std::polar (1.0, -2.0 * kPi * f / hz);
    return db (std::abs ((1.0 + ff * z) / (1.0 - fb * z)));
}

bool smokeCombFormula()
{
    struct Setting { double fb, ff; };
    const Setting settings[] = { { 0.8, 0.0 }, { -0.8, -1.0 }, { 0.0, 1.0 }, { 0.5, -0.5 } };
    bool ok = true;
    for (double rate : { kVoiceRate, kRate })
    {
        double worst = 0.0;
        int tones = 0;
        char where[128] = {};
        for (const Setting& st : settings)
            for (double hz : { 73.2, 440.0 })
            {
                // Log-spaced tones from 20 Hz to 8 kHz, plus the peaks and dips
                // (multiples of hz/2) of the first harmonics and near 8 kHz.
                std::vector<double> freqs;
                for (int i = 0; i < 40; ++i)
                    freqs.push_back (20.0 * std::pow (400.0, i / 39.0));
                const int top = static_cast<int> (8000.0 / (0.5 * hz));
                for (int k = 1; k <= top; ++k)
                    if (k <= 12 || k > top - 6)
                        freqs.push_back (0.5 * hz * k);
                double maxDb = -1.0e9;
                for (double f : freqs)
                    maxDb = std::max (maxDb, combFormulaDb (f, hz, st.fb, st.ff));
                for (double f : freqs)
                {
                    const double want = combFormulaDb (f, hz, st.fb, st.ff);
                    if (want < maxDb - 30.0)
                        continue;  // inside a notch: the dB error is meaningless there
                    std::vector<float> store (static_cast<size_t> (CombFfState::capacityFor (rate)));
                    CombFfState c;
                    c.buf = store.data();
                    c.capacity = static_cast<int> (store.size());
                    c.reset();
                    const double passes = std::fabs (st.fb) > 0.0 ? 40.0 : 3.0;
                    const size_t settle = static_cast<size_t> (rate * (passes / hz + 0.02));
                    const size_t n = static_cast<size_t> (rate * std::max (0.1, 8.0 / f));
                    std::vector<double> y (settle + n);
                    Sine s (f, rate);
                    for (size_t i = 0; i < y.size(); ++i)
                        y[i] = c.process (static_cast<float> (s.next()), hz, st.fb, st.ff, rate);
                    const double got = db (toneAmp (y, settle, n, f, rate));
                    ++tones;
                    if (std::fabs (got - want) > worst)
                    {
                        worst = std::fabs (got - want);
                        std::snprintf (where, sizeof (where), "fb %.1f ff %.1f hz %.1f at %.0f Hz: %.2f vs %.2f dB",
                                       st.fb, st.ff, hz, f, got, want);
                    }
                }
            }
        ok = check (worst <= 0.5, "fx_comb_ff at %.0f kHz: %d sine tones below 8 kHz (fb/ff 0.8/0, -0.8/-1, 0/1, 0.5/-0.5; "
                    "hz 73.2 and 440) match (1 + ff Z) / (1 - fb Z) within %.3f dB (gate 0.5; worst %s)",
                    rate / 1000.0, tones, worst, where) && ok;
    }
    return ok;
}

bool smokeCombVm()
{
    bool ok = true;
    double macros[MacroSmoother::kNumMacros] = {};
    // Peak: fb 0.8 at hz gives 1 / (1 - 0.8) = +14.0 dB; notch: ff 1 at hz / 2 gives 0.
    const PatchStrings ps = { "0;", "a = fx_comb_ff(0, osc_sine(0, 440), 440, 0.8, 0) * 0.1;"
                                    " b = fx_comb_ff(1, osc_sine(1, 220), 440, 0, 1);"
                                    " outl = a; outr = b;", "" };
    auto v = std::make_unique<EelVoice> (ps, kVoiceRate, 3u);
    if (! check (v->isValid(), "fx_comb_ff voice patch compiles: %s", v->compileError().c_str()))
        return false;
    v->noteOn (440.0, 69.0, 1.0);
    std::vector<double> a, b;
    for (int i = 0; i < 48000; ++i)
    {
        float l = 0.0f, r = 0.0f;
        v->renderSample (440.0, 1.0, 69.0, 1.0, macros, Transport {}, 0.0, l, r);
        a.push_back (l * 10.0);
        b.push_back (r);
    }
    const double peak = db (toneAmp (a, 24000, 24000, 440.0, kVoiceRate));
    const double notch = db (toneAmp (b, 24000, 24000, 220.0, kVoiceRate));
    ok = check (std::fabs (peak - 13.98) < 0.3 && notch < -40.0,
                "fx_comb_ff through the VM: fb 0.8 at hz is %+.2f dB (want +13.98), ff 1 at hz/2 is %.1f dB (want a notch)",
                peak, notch) && ok;

    // A voice whose code doesn't name it gets no delay memory: the slot passes its input.
    {
        const PatchStrings plain = { "0;", "out = 0;", "" };
        auto p = std::make_unique<EelVoice> (plain, kVoiceRate, 3u);
        EEL_F args[5] = { 0, 0.25, 440, 0.8, 0.5 };
        EEL_F* ptrs[5] = { &args[0], &args[1], &args[2], &args[3], &args[4] };
        const double out = fnFxCombFf (&p->rackContext(), 5, ptrs);
        ok = check (p->rack().combFf[0].buf == nullptr && out == 0.25 && v->rack().combFf[1].buf != nullptr,
                    "fx_comb_ff delay memory exists only for code that names it (else the input passes: %.2f)", out) && ok;
    }
    return ok;
}

// Energy above 8 kHz while hz moves, on a 1 kHz sine: a click or zipper puts
// broadband energy there; the glide itself only bends the tone's pitch.
bool smokeCombMoving()
{
    bool ok = true;
    const double rate = kVoiceRate;
    auto run = [rate] (bool smoothed, int mode, double& hfDb)
    {
        std::vector<float> store (static_cast<size_t> (CombFfState::capacityFor (rate)));
        CombFfState c;
        c.buf = store.data();
        c.capacity = static_cast<int> (store.size());
        c.reset();
        TestBiquad h1 = TestBiquad::make (1, 8000.0, 0.5412, rate), h2 = TestBiquad::make (1, 8000.0, 1.3066, rate);
        Sine s (1000.0, rate);
        double hf = 0.0, sig = 0.0;
        const int n = static_cast<int> (rate * 1.2);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / rate;
            double hz = 0.0;
            if (mode == 0)
                hz = t < 0.5 ? 440.0 : 73.2;                                   // a jump
            else
                hz = 200.0 * std::pow (1.5, std::sin (2.0 * kPi * 2.0 * t));   // a 2 Hz sweep, 133-300 Hz
            if (! smoothed)
            {
                c.smoothRate = rate;
                c.smooth = 1.0;  // control: no glide
            }
            const double y = c.process (static_cast<float> (0.25 * s.next()), hz, 0.8, 0.5, rate);
            const double h = h2.process (h1.process (y));
            if (i > static_cast<int> (rate * 0.3))
            {
                hf = std::max (hf, std::fabs (h));
                sig = std::max (sig, std::fabs (y));
            }
        }
        hfDb = db (hf / sig);
    };
    double jump = 0.0, sweep = 0.0, jumpRaw = 0.0, sweepRaw = 0.0;
    run (true, 0, jump);
    run (true, 1, sweep);
    run (false, 0, jumpRaw);
    run (false, 1, sweepRaw);
    ok = check (jump < -60.0 && sweep < -60.0,
                "fx_comb_ff moving hz, 1 kHz sine, fb 0.8 ff 0.5 at 96 kHz: peak above 8 kHz %.1f dB (jump 440 -> 73.2 Hz) "
                "and %.1f dB (2 Hz sweep) below the signal (gate -60; with the glide turned off: %.1f and %.1f dB)",
                jump, sweep, jumpRaw, sweepRaw) && ok;
    return ok;
}

bool smokeCombBounds()
{
    bool safe = true;
    int cases = 0;
    double macros[MacroSmoother::kNumMacros] = {};
    // (a) The state, each argument at each bad value, then normal again.
    for (int arg = 0; arg < 4; ++arg)
        for (double rate : { kVoiceRate, kRate })
            for (double bad : kBad)
            {
                std::vector<float> store (static_cast<size_t> (CombFfState::capacityFor (rate)));
                CombFfState c;
                c.buf = store.data();
                c.capacity = static_cast<int> (store.size());
                c.reset();
                ++cases;
                for (int i = 0; i < 6000; ++i)
                {
                    double a[4] = { 0.5 * std::sin (i * 0.01), 300.0, 0.9, -0.7 };
                    const bool during = i >= 1000 && i < 3000;
                    if (during) a[arg] = bad;
                    if (i == 3000)
                        c.reset();  // after a 1e30 input the ringing is legitimately huge
                    const float y = c.process (static_cast<float> (a[0]), a[1], a[2], a[3], rate);
                    // fb 0.95 and ff 1 at most: |H| <= 2 / 0.05 = 40, so |y| <= 20 for |in| <= 0.5.
                    safe = safe && finiteF (y) && ((arg == 0 && during) || std::fabs (y) <= 21.0f);
                }
            }
    // (b) Straight into the wrapper, through a voice that has the memory.
    {
        const PatchStrings ps = { "0;", "out = fx_comb_ff(0, osc_saw(0, freq), 220, 0.5, 0.5);", "" };
        auto v = std::make_unique<EelVoice> (ps, kVoiceRate, 3u);
        safe = safe && v->isValid();
        for (int arg = 0; arg < 5; ++arg)
            for (double bad : kBad)
            {
                ++cases;
                v->resetState();
                for (int i = 0; i < 3000; ++i)
                {
                    EEL_F a[5] = { 1, 0.5 * std::sin (i * 0.02), 150, -0.9, 1 };
                    if (i >= 1000) a[arg] = bad;
                    EEL_F* ptrs[5] = { &a[0], &a[1], &a[2], &a[3], &a[4] };
                    const double y = fnFxCombFf (&v->rackContext(), 5, ptrs);
                    safe = safe && finiteF (static_cast<float> (y)) && (arg == 1 || std::fabs (y) <= 21.0);
                }
            }
    }
    // (c) Through the VM: slots, hz and amounts EEL2 can make, voice and master.
    const char* voiceCases[] = {
        "big = pow(10, 30); out = fx_comb_ff(0, osc_saw(0, freq), big, big, -big) + fx_comb_ff(1, noise(), -big, -big, big);",
        "out = fx_comb_ff(99, osc_saw(0, freq), 0, 0.95, 1) + fx_comb_ff(-3, noise(), 1000000, -0.95, -1);",
        "out = fx_comb_ff(0, osc_saw(0, freq), 30 + 20000 * rand(1), 2 * rand(1) - 1, 2 * rand(1) - 1);",
    };
    for (const char* code : voiceCases)
    {
        const PatchStrings ps = { "0;", code, "" };
        auto v = std::make_unique<EelVoice> (ps, kVoiceRate, 5u);
        safe = safe && v->isValid();
        v->noteOn (110.0, 45.0, 0.9);
        ++cases;
        for (int i = 0; i < 20000 && safe; ++i)
        {
            const float s = v->renderSample (110.0, 1.0, 45.0, 0.9, macros);
            safe = safe && finiteF (s) && std::fabs (s) < 100.0f;
        }
    }
    {
        auto m = makeMaster ("big = pow(10, 30); outl = fx_comb_ff(0, inl, big, -big, big); outr = fx_comb_ff(1, inr, 10, 0.95, 1);");
        safe = safe && m->isValid();
        ++cases;
        for (int i = 0; i < 20000 && safe; ++i)
        {
            float ol = 0.0f, orr = 0.0f;
            const float in = static_cast<float> (0.5 * std::sin (i * 0.03));
            m->renderSample (in, in, ol, orr, macros);
            safe = safe && finiteF (ol) && finiteF (orr) && std::fabs (ol) < 100.0f && std::fabs (orr) < 100.0f;
        }
    }
    return check (safe, "fx_comb_ff bounds/NaN: %d cases of NaN, Inf and +/-1e30 in every argument of the state and the "
                  "function, absurd slots and VM extremes in voice and master: finite, |out| <= 40 x |in|", cases);
}

// ===========================================================================
// bus_mb

// Writes 19 settings at `base` in the VM's mem[].
bool setMb (void* vm, int base, const double (&v)[MbCompState::kNumValues])
{
    bool ok = true;
    for (int i = 0; i < MbCompState::kNumValues; ++i)
        ok = poke (vm, base + i, v[i]) && ok;
    return ok;
}

// Neutral: thresholds at the ends, ratios 1, gains 0.
void neutralMb (double (&v)[MbCompState::kNumValues], double f1, double f2)
{
    const double n[MbCompState::kNumValues] = { f1, f2, 10, 100, -90, 12, 1, 1, 0, -90, 12, 1, 1, 0, -90, 12, 1, 1, 0 };
    std::copy (std::begin (n), std::end (n), v);
}

// Amplitude gain of the state on one sine (L and R fed the same), after settling.
double mbToneGain (MbCompState& st, void* vm, int base, double hz, double amp, double rate, double settleS, double measureS,
                   double mix = 1.0, double ampR = -1.0, double* gainR = nullptr)
{
    st.init (rate);
    const size_t settle = static_cast<size_t> (rate * settleS);
    const size_t n = static_cast<size_t> (rate * measureS);
    std::vector<double> yl (settle + n), yr (settle + n);
    Sine s (hz, rate);
    const double ar = ampR < 0.0 ? amp : ampR;
    for (size_t i = 0; i < yl.size(); ++i)
    {
        const double x = s.next();
        float ol = 0.0f, orr = 0.0f;
        st.process (vm, static_cast<float> (amp * x), static_cast<float> (ar * x), base, mix, ol, orr);
        yl[i] = ol;
        yr[i] = orr;
    }
    if (gainR != nullptr)
        *gainR = toneAmp (yr, settle, n, hz, rate) / ar;
    return toneAmp (yl, settle, n, hz, rate) / amp;
}

// What the formula says for that tone: each band's level is the tone's level
// through that band, its gain comes from the band's curve, and the bands sum
// with their complex responses.
double mbExpectedGain (const MbCompState& st, double hz, double amp, double rate, double mix = 1.0)
{
    const auto lp1 = biquadResponse (st.lp1, hz, rate), hp1 = biquadResponse (st.hp1, hz, rate);
    const auto lp2 = biquadResponse (st.lp2, hz, rate), hp2 = biquadResponse (st.hp2, hz, rate);
    const auto lo = lp1 * lp1 * biquadResponse (st.ap2, hz, rate);
    const auto rest = hp1 * hp1;
    const auto mid = rest * lp2 * lp2;
    const auto hi = rest * hp2 * hp2;
    const std::complex<double> band[3] = { lo, mid, hi };
    std::complex<double> sum = 0.0;
    for (int b = 0; b < 3; ++b)
    {
        const double g = std::pow (10.0, st.curveDb (b, db (amp * std::abs (band[b]))) / 20.0);
        sum += band[b] * (1.0 + mix * (g - 1.0));
    }
    return std::abs (sum);
}

bool smokeMb()
{
    bool ok = true;
    double macros[MacroSmoother::kNumMacros] = {};
    auto host = makeMaster ("outl = bus_mb(inl, inr, 100, 1); outr = bus_mb_r();");
    if (! check (host->isValid(), "bus_mb master patch compiles: %s", host->compileError().c_str()))
        return false;
    void* vm = host->rackContext().vm;
    constexpr int kBase = 100;

    // Fast log2 and 2^x against std::.
    {
        double worstLog = 0.0, worstExp = 0.0;
        for (double x = 1.0e-9; x < 1.0e4; x *= 1.0137)
            worstLog = std::max (worstLog, std::fabs (mbLog2 (x) - std::log2 (x)));
        for (double x = -40.0; x <= 40.0; x += 0.0173)
            worstExp = std::max (worstExp, std::fabs (mbExp2 (x) / std::exp2 (x) - 1.0));
        ok = check (worstLog < 1.0e-7 && worstExp < 1.0e-6,
                    "bus_mb fast log2 within %.1e of std::log2 (1e-9..1e4), fast 2^x within %.1e relative (-40..40)",
                    worstLog, worstExp) && ok;
    }

    // Neutral settings: the three bands sum flat, 20 Hz - 20 kHz, at 44.1, 48 and 96 kHz.
    {
        double worst = 0.0;
        int tones = 0;
        const double pairs[][2] = { { 120.0, 2500.0 }, { 20.0, 20000.0 }, { 1000.0, 1000.0 }, { 300.0, 6000.0 } };
        for (double rate : { 44100.0, kRate, 96000.0 })
            for (const auto& pr : pairs)
            {
                double v[MbCompState::kNumValues];
                neutralMb (v, pr[0], pr[1]);
                ok = setMb (vm, kBase, v) && ok;
                MbCompState st;
                for (int i = 0; i < 31; ++i)
                {
                    const double f = 20.0 * std::pow (1000.0, i / 30.0);
                    if (f >= 0.45 * rate)
                        continue;
                    const double g = mbToneGain (st, vm, kBase, f, 0.5, rate, 0.3, std::max (0.05, 6.0 / f));
                    worst = std::max (worst, std::fabs (db (g)));
                    ++tones;
                }
            }
        ok = check (worst <= 0.1, "bus_mb with ratios 1 and gains 0 sums flat: %d tones 20 Hz-20 kHz at 44.1/48/96 kHz, "
                    "crossovers 120/2500, 20/20000, 1000/1000, 300/6000 Hz: within %.4f dB (gate 0.1)", tones, worst) && ok;
    }

    // Static curves: three different bands, sine tones at 55, 880, 7040 Hz from -80 to 0 dBFS.
    {
        const double v[MbCompState::kNumValues] = { 120, 2500, 10, 100,
                                                    -50, -20, 3, 4, 3,      // low
                                                    -60, -30, 4, 6, 6,      // mid
                                                    -40, -10, 2, 10, -2 };  // high
        ok = setMb (vm, kBase, v) && ok;
        const double timings[][2] = { { 10.0, 100.0 }, { 5.0, 150.0 }, { 1.0, 100.0 }, { 50.0, 50.0 } };
        for (const auto& tm : timings)
        {
            double vv[MbCompState::kNumValues];
            std::copy (std::begin (v), std::end (v), vv);
            vv[2] = tm[0];
            vv[3] = tm[1];
            ok = setMb (vm, kBase, vv) && ok;
            char detail[256] = {};
            double worstAll = 0.0;
            for (double hz : { 55.0, 880.0, 7040.0 })
            {
                MbCompState st;
                double worst = 0.0, at = 0.0;
                for (double level = -80.0; level <= 0.01; level += 5.0)
                {
                    const double amp = std::pow (10.0, level / 20.0);
                    const double got = db (mbToneGain (st, vm, kBase, hz, amp, kRate, 0.8, 0.4));
                    const double want = db (mbExpectedGain (st, hz, amp, kRate));
                    if (std::fabs (got - want) > worst)
                    {
                        worst = std::fabs (got - want);
                        at = level;
                    }
                }
                worstAll = std::max (worstAll, worst);
                const size_t used = std::strlen (detail);
                std::snprintf (detail + used, sizeof (detail) - used, "%s%.0f Hz %.2f dB (at %.0f dBFS)", used ? ", " : "",
                               hz, worst, at);
            }
            if (tm[0] == 10.0)
                ok = check (worstAll <= 0.5, "bus_mb static curves, attack 10 ms / release 100 ms, 17 levels -80..0 dBFS "
                            "per tone, against the formula: worst %s (gate 0.5)", detail) && ok;
            else
                std::printf ("[INFO] bus_mb static curves with attack %.0f ms / release %.0f ms: worst %s\n", tm[0], tm[1],
                             detail);
        }
    }

    // Channels are compressed separately: a loud L doesn't turn a quiet R down.
    {
        const double v[MbCompState::kNumValues] = { 120, 2500, 10, 100, -50, -20, 3, 4, 3, -60, -30, 4, 6, 6, -40, -10, 2, 10, -2 };
        ok = setMb (vm, kBase, v) && ok;
        MbCompState st, ref;
        double gr = 0.0;
        const double gl = mbToneGain (st, vm, kBase, 880.0, 0.5, kRate, 0.8, 0.4, 1.0, 0.003, &gr);
        const double wantL = mbExpectedGain (st, 880.0, 0.5, kRate), wantR = mbExpectedGain (st, 880.0, 0.003, kRate);
        ok = check (std::fabs (db (gl / wantL)) < 0.5 && std::fabs (db (gr / wantR)) < 0.5,
                    "bus_mb channels are not linked: L at -6 dBFS gets %+.1f dB (curve %+.1f), R at -50 dBFS %+.1f dB "
                    "(curve %+.1f)", db (gl), db (wantL), db (gr), db (wantR)) && ok;

        // mix 0.5 scales each band's linear gain change by half.
        const double half = mbToneGain (ref, vm, kBase, 880.0, 0.01, kRate, 0.8, 0.4, 0.5);
        const double wantHalf = mbExpectedGain (ref, 880.0, 0.01, kRate, 0.5);
        ok = check (std::fabs (db (half / wantHalf)) < 0.3, "bus_mb mix 0.5: %+.2f dB at -40 dBFS (formula %+.2f)",
                    db (half), db (wantHalf)) && ok;
    }

    // Mix 0 and unreadable settings end at the dry input bit for bit (after the 10 ms fade).
    {
        const double v[MbCompState::kNumValues] = { 120, 2500, 10, 100, -50, -20, 3, 4, 3, -60, -30, 4, 6, 6, -40, -10, 2, 10, -2 };
        ok = setMb (vm, kBase, v) && ok;
        int exactCases = 0, cases = 0;
        const double bases[] = { kBase, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                                 -1.0e30, -3.0, 65530.0, 1.0e30 };
        for (double b : bases)
        {
            ++cases;
            MbCompState st;
            st.init (kRate);
            bool exact = true;
            for (int i = 0; i < 4800; ++i)
            {
                const float l = static_cast<float> (0.3 * std::sin (i * 0.03)), r = static_cast<float> (-0.2 * std::sin (i * 0.05));
                float ol = 0.0f, orr = 0.0f;
                st.process (vm, l, r, b, b == kBase ? 0.0 : 1.0, ol, orr);
                if (i > 600)
                    exact = exact && sameBitsF (ol, l) && sameBitsF (orr, r);
            }
            exactCases += exact ? 1 : 0;
        }
        ok = check (exactCases == cases, "bus_mb mix 0, and base NaN, Inf, -1e30, -3, 65530 (past the end), 1e30, all end "
                    "at the dry input bit for bit after the 10 ms fade (%d/%d)", exactCases, cases) && ok;
    }

    // Settings are read at control rate: a new output gain lands within 16 samples, not before the next read.
    {
        double v[MbCompState::kNumValues];
        neutralMb (v, 120, 2500);
        ok = setMb (vm, kBase, v) && ok;
        MbCompState st;
        st.init (kRate);
        int changedAt = -1;
        for (int i = 0; i < 2000; ++i)
        {
            if (i == 1000)
                for (int b = 0; b < 3; ++b)
                    poke (vm, kBase + 8 + 5 * b, 6.0);
            float ol = 0.0f, orr = 0.0f;
            st.process (vm, 0.25f, 0.25f, kBase, 1.0, ol, orr);  // DC: only the low band carries it
            if (i >= 1000 && changedAt < 0 && std::fabs (ol) > 0.4f)
                changedAt = i - 1000;
        }
        ok = check (changedAt >= 0 && changedAt < MbCompState::kInterval,
                    "bus_mb reads its settings every %d samples: a new gain took effect after %d samples",
                    MbCompState::kInterval, changedAt) && ok;
    }

    // Bounds and NaN: every field at every bad value in mem[], every argument straight into the function.
    {
        bool safe = true, poked = true;
        int cases = 0;
        const double good[MbCompState::kNumValues] = { 120, 2500, 5, 150, -50, -20, 3, 4, 3, -60, -30, 4, 6, 6, -40, -10, 2, 10, -2 };
        for (int field = 0; field < MbCompState::kNumValues; ++field)
            for (double bad : kBad)
            {
                ok = setMb (vm, kBase, good) && ok;
                MbCompState st;
                st.init (kRate);
                ++cases;
                for (int i = 0; i < 4000; ++i)
                {
                    if (i == 1000)
                        poked = poke (vm, kBase + field, bad) && poked;
                    const double x = (i % 2000 < 1000 ? 0.9 : 1.0e-4) * std::sin (i * 0.07);
                    float ol = 0.0f, orr = 0.0f;
                    st.process (vm, static_cast<float> (x), static_cast<float> (-x), kBase, 1.0, ol, orr);
                    // Gain is at most +48 dB output gain plus (12 - -90) * 0.99 upward: finite whatever the data.
                    safe = safe && finiteF (ol) && finiteF (orr) && std::fabs (ol) < 1.0e4f && std::fabs (orr) < 1.0e4f;
                }
            }
        ok = setMb (vm, kBase, good) && ok;
        for (int arg = 0; arg < 4; ++arg)
            for (double bad : kBad)
            {
                ++cases;
                for (int i = 0; i < 2000; ++i)
                {
                    EEL_F a[4] = { 0.5 * std::sin (i * 0.05), 0.3 * std::sin (i * 0.02), kBase, 1 };
                    if (i >= 500) a[arg] = bad;
                    EEL_F* ptrs[4] = { &a[0], &a[1], &a[2], &a[3] };
                    const double l = fnBusMb (&host->rackContext(), 4, ptrs);
                    const float r = host->rack().mbRight;
                    safe = safe && finiteF (static_cast<float> (l)) && finiteF (r);
                }
            }
        for (const char* code : { "big = pow(10, 30); outl = bus_mb(inl * big, inr, -big, big); outr = bus_mb_r();",
                                  "memset(0, 0, 40); outl = bus_mb(inl, inr, 0, 1); outr = bus_mb_r();",
                                  "big = pow(10, 30); i = 0; loop(19, mem[i] = big * (i % 2 ? 1 : -1); i += 1); outl = bus_mb(inl, inr, 0, 1); outr = bus_mb_r();",
                                  "outl = bus_mb(inl, inr, 65520, 2); outr = bus_mb_r();" })
        {
            auto m = makeMaster (code);
            safe = safe && m->isValid();
            ++cases;
            for (int i = 0; i < 20000 && safe; ++i)
            {
                float ol = 0.0f, orr = 0.0f;
                const float in = static_cast<float> (0.8 * std::sin (i * 0.05));
                m->renderSample (in, -in, ol, orr, macros);
                safe = safe && finiteF (ol) && finiteF (orr);
            }
        }
        ok = check (poked, "bus_mb bad-data cases really hold NaN and Inf in mem[]") && ok;
        ok = check (safe, "bus_mb bounds/NaN: %d cases of NaN, Inf and +/-1e30 in each of the 19 settings and every "
                    "argument, garbage and out-of-range mem[]: finite", cases) && ok;
    }
    return ok;
}

} // namespace

bool smokePhase2()
{
    std::printf ("== Phase 2 sound-tool smoketests ==\n");
    bool ok = true;
    ok = smokeReverb() && ok;
    ok = smokeCombFormula() && ok;
    ok = smokeCombVm() && ok;
    ok = smokeCombMoving() && ok;
    ok = smokeCombBounds() && ok;
    ok = smokeMb() && ok;
    return ok;
}

} // namespace vibesynth_cli
