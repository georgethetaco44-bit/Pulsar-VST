// M7 smoketests (SPEC §4.2, §4.2.1, §9.1), part of vibesynth_cli
// --smoketest:
//   smokeDecimator   the half-band decimator: measured rejection at 0.55,
//                    0.75 and 0.95 x host rate, passband ripple to 20 kHz,
//                    group delay at 1 kHz (and the latency it adds on the
//                    host timeline).
//   smokeAliasAB     fx_fold(osc_sine) over a stepped pitch sweep through
//                    PatchPlayer at --os 1 and 2: inharmonic energy (FFT,
//                    everything not within 6 bins of a harmonic) and the
//                    improvement 2x brings.
//   smokeFilters     the own TPT SVF and ZDF ladder: prewarp accuracy,
//                    cutoff at 3 points, resonance peak, HP/BP/ladder
//                    shapes, ladder self-oscillation (bounded), no NaN on
//                    1 ms full-range sweeps, and the zipper test (a fast
//                    cutoff sweep on a sine has no 16-sample comb).
//   smokeVoiceModes  mono legato (no trig, no init, gate held), return to
//                    the held note, retrigger in place, glide time,
//                    pitch-bend range and smoothing, mw in voice and
//                    master, CC 123, and Patch/Poly/Mono resolution.

#include "SmokeM7.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "CliCheck.h"
#include "engine/Filters.h"
#include "engine/Oversampler.h"
#include "patch/Patch.h"
#include "patch/PatchEngine.h"
#include "patch/PatchPlayer.h"
#include "patch/PatchValidator.h"

namespace vibesynth_cli {

namespace {

using vibesynth::PatchPlayer;
using vibesynth::PatchProgram;
using Mode = vibesynth::PatchPlayer::VoiceMode;

constexpr double kHostRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

double db (double gain)
{
    return 20.0 * std::log10 (std::max (gain, 1.0e-30));
}

bool finite (double v)
{
    return vibesynth::isFiniteBits (v);
}

// ---------------------------------------------------------------------------
// Spectra: radix-2 FFT, Blackman-Harris window (-92 dB sidelobes, main lobe
// +-4 bins).

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

// Power spectrum (bins 0..n/2) of x[start, start + n), windowed.
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

// Energy outside +-6 bins of every harmonic of f0, relative to all energy
// above 20 Hz, in dB.
double inharmonicDb (const std::vector<float>& x, size_t start, size_t n, double f0, double sr)
{
    const std::vector<double> p = powerSpectrum (x, start, n);
    const double binHz = sr / static_cast<double> (n);
    std::vector<bool> harmonic (p.size(), false);
    for (int h = 1; h * f0 < 0.5 * sr; ++h)
    {
        const long c = std::lround (h * f0 / binHz);
        for (long k = c - 6; k <= c + 6; ++k)
            if (k >= 0 && k < (long) p.size())
                harmonic[(size_t) k] = true;
    }
    double total = 0.0, inh = 0.0;
    for (size_t k = (size_t) std::ceil (20.0 / binHz); k < p.size(); ++k)
    {
        total += p[k];
        if (! harmonic[k])
            inh += p[k];
    }
    return 10.0 * std::log10 (std::max (inh, 1.0e-300) / std::max (total, 1.0e-300));
}

// ---------------------------------------------------------------------------
// Player helpers.

struct Ev
{
    int samplePosition;
    const uint8_t* data;
    int numBytes;
};

// A MIDI script: events at host samples, owning their bytes.
struct Script
{
    std::vector<std::array<uint8_t, 3>> bytes;
    std::vector<int> at;

    void add (int sample, uint8_t b0, uint8_t b1, uint8_t b2)
    {
        bytes.push_back ({ b0, b1, b2 });
        at.push_back (sample);
    }
    void on (int sample, int note) { add (sample, 0x90, (uint8_t) note, 100); }
    void off (int sample, int note) { add (sample, 0x80, (uint8_t) note, 0); }

    std::vector<Ev> events() const
    {
        std::vector<Ev> e;
        for (size_t i = 0; i < at.size(); ++i)
            e.push_back ({ at[i], bytes[i].data(), 3 });
        std::stable_sort (e.begin(), e.end(), [] (const Ev& a, const Ev& b)
                          { return a.samplePosition < b.samplePosition; });
        return e;
    }
};

std::unique_ptr<PatchProgram> buildProgram (const vibesynth::Patch& patch, int os)
{
    std::string error;
    auto prog = PatchProgram::build (patch, kHostRate, error, os);
    if (prog == nullptr)
        check (false, "%s builds: %s", patch.name.c_str(), error.c_str());
    return prog;
}

// Plays `script` for `total` host samples in `chunk`-sample chunks; after
// each chunk calls after(endSample). Collects the left output.
std::vector<float> play (PatchPlayer& player, PatchProgram& prog, const Script& script, int total,
                         int chunk, Mode mode, double glide,
                         const std::function<void (int)>& after = nullptr)
{
    float targets[PatchPlayer::kNumMacros] = {};
    for (int m = 0; m < PatchPlayer::kNumMacros; ++m)
        targets[m] = static_cast<float> (prog.patch().macroDefault (m));
    // Programs are shared between runs: start every run from fresh VMs.
    for (int i = 0; i < PatchProgram::kNumVoices; ++i)
        prog.voice (i).resetState();
    prog.master().resetState();
    player.setProgram (&prog);
    player.prepare (kHostRate, std::max (chunk, 512), targets);
    const std::vector<Ev> events = script.events();
    auto it = events.cbegin();
    std::vector<float> out ((size_t) total, 0.0f);
    for (int start = 0; start < total; start += chunk)
    {
        const int n = std::min (chunk, total - start);
        player.setVoiceControls (mode, glide);
        player.renderChunk (n, targets, it, events.cend(), start);
        std::copy (player.outL(), player.outL() + n, out.begin() + start);
        if (after)
            after (start + n);
    }
    return out;
}

// The first slot holding a voice, or -1.
int firstSlot (const PatchPlayer& player)
{
    for (int s = 0; s < PatchPlayer::kNumVoiceObjects; ++s)
        if (player.slotVoiceIndex (s) >= 0)
            return s;
    return -1;
}

double voiceVar (PatchProgram& prog, const PatchPlayer& player, int slot, const char* name)
{
    const int index = player.slotVoiceIndex (slot);
    if (index < 0)
        return -1.0;
    const EEL_F* v = prog.voice (index).variable (name);
    return v != nullptr ? static_cast<double> (*v) : -1.0;
}

double cents (double f, double ref)
{
    return 1200.0 * std::log2 (f / ref);
}

// ---------------------------------------------------------------------------
// Filter probing: complex gain at f (Hann-weighted correlation of the
// output against the input, after a settle).

std::complex<double> probe (const std::function<float (float)>& process, double f, double fs,
                            double amp = 0.1)
{
    const double w = 2.0 * kPi * f / fs;
    const int settle = (int) (0.3 * fs);
    const int n = (int) std::max (0.25 * fs, 40.0 * fs / f);
    std::complex<double> x (0.0, 0.0), y (0.0, 0.0);
    for (int i = 0; i < settle + n; ++i)
    {
        const double in = amp * std::sin (w * i);
        const double out = static_cast<double> (process (static_cast<float> (in)));
        if (i < settle)
            continue;
        const int k = i - settle;
        const double win = 0.5 - 0.5 * std::cos (2.0 * kPi * k / n);
        const std::complex<double> e (std::cos (w * i), -std::sin (w * i));
        x += win * in * e;
        y += win * out * e;
    }
    return y / x;
}

// Bisection in log f on [lo, hi] for the point where pred flips from true
// (below) to false.
double bisectHz (double lo, double hi, const std::function<bool (double)>& below)
{
    for (int i = 0; i < 24; ++i)
    {
        const double mid = std::sqrt (lo * hi);
        if (below (mid))
            lo = mid;
        else
            hi = mid;
    }
    return std::sqrt (lo * hi);
}

double normToHz (double c)
{
    return 20.0 * std::pow (1000.0, c);
}

} // namespace

// ===========================================================================

bool smokeDecimator()
{
    vibesynth::HalfbandDecimator d;
    const double vr = 2.0 * kHostRate;
    bool ok = check (d.numCoefs() >= 6 && d.numCoefs() <= 12,
                     "half-band design: %d allpass coefficients (%.0f dB, transition %.2f of 2x rate)",
                     d.numCoefs(), vibesynth::HalfbandDecimator::kDesignAttenuationDb,
                     vibesynth::HalfbandDecimator::kDesignTransition);

    // Measured rejection: a full-scale sine at the 2x rate, output RMS.
    for (const double rel : { 0.55, 0.75, 0.95 })
    {
        d.reset();
        const double w = 2.0 * kPi * rel * kHostRate / vr;
        const int settle = 8192, n = 65536;
        double sum = 0.0;
        for (int m = 0; m < settle + n; ++m)
        {
            const float a = static_cast<float> (std::sin (w * (2 * m)));
            const float b = static_cast<float> (std::sin (w * (2 * m + 1)));
            const double y = d.process (a, b);
            if (m >= settle)
                sum += y * y;
        }
        const double rej = db (std::sqrt (sum / n) / std::sqrt (0.5));
        double mag = 0.0, gd = 0.0;
        d.response (rel * kHostRate / vr, mag, gd);
        ok = check (rej <= -90.0, "decimator rejection at %.2f x host rate (%.1f kHz): %.1f dB measured, "
                    "%.1f dB analytic (want <= -90)", rel, rel * kHostRate / 1000.0, rej, db (mag)) && ok;
    }

    // Passband: Hann-correlated amplitude at 40 log-spaced points 20 Hz-20 kHz.
    double worst = 0.0, worstAt = 0.0, worstAnalytic = 0.0;
    for (int i = 0; i < 40; ++i)
    {
        const double f = 20.0 * std::pow (1000.0, i / 39.0);
        d.reset();
        const double w = 2.0 * kPi * f / vr;
        const int settle = 4096, n = 96000;
        std::complex<double> y (0.0, 0.0);
        double wsum = 0.0;
        for (int m = 0; m < settle + n; ++m)
        {
            const double out = d.process (static_cast<float> (std::sin (w * (2 * m))),
                                          static_cast<float> (std::sin (w * (2 * m + 1))));
            if (m < settle)
                continue;
            const int k = m - settle;
            const double win = 0.5 - 0.5 * std::cos (2.0 * kPi * k / n);
            y += win * out * std::complex<double> (std::cos (2.0 * w * m), -std::sin (2.0 * w * m));
            wsum += win;
        }
        const double gainDb = db (2.0 * std::abs (y) / wsum);
        if (std::fabs (gainDb) > std::fabs (worst))
        {
            worst = gainDb;
            worstAt = f;
        }
        double mag = 0.0, gd = 0.0;
        d.response (f / vr, mag, gd);
        worstAnalytic = std::max (worstAnalytic, std::fabs (db (mag)));
    }
    ok = check (std::fabs (worst) <= 0.01,
                "decimator passband 20 Hz-20 kHz: max deviation %.5f dB measured (at %.0f Hz), "
                "%.2e dB analytic (want <= 0.01)", worst, worstAt, worstAnalytic) && ok;

    // Group delay at 1 kHz. The filter's own delay is D 2x-samples; on the
    // host timeline (host sample m <-> voice samples 2m, 2m+1, and output m
    // includes voice sample 2m+1) the added latency is (D - 1) / 2 host
    // samples. Measured: phase of the output against a host-rate sine.
    double mag = 0.0, gd = 0.0;
    d.response (1000.0 / vr, mag, gd);
    {
        d.reset();
        const double w = 2.0 * kPi * 1000.0 / vr;
        std::complex<double> y (0.0, 0.0);
        const int settle = 4096, n = 48000;
        for (int m = 0; m < settle + n; ++m)
        {
            const double out = d.process (static_cast<float> (std::sin (w * (2 * m))),
                                          static_cast<float> (std::sin (w * (2 * m + 1))));
            if (m >= settle)
                y += out * std::complex<double> (std::cos (2.0 * w * m), -std::sin (2.0 * w * m));
        }
        // y ~ A/2 * e^{-j(pi/2 + phi)} for out = A sin(wh m - phi).
        double phi = -std::arg (y) - 0.5 * kPi;
        while (phi < 0.0) phi += 2.0 * kPi;
        const double latency = phi / (2.0 * w);
        ok = check (std::fabs (latency - (gd - 1.0) / 2.0) < 0.01,
                    "decimator group delay at 1 kHz: %.3f samples at 2x (%.3f host samples, %.1f us); "
                    "latency added on the host timeline %.3f host samples (%.1f us at 48 kHz) measured, "
                    "%.3f analytic", gd, gd / 2.0, 1.0e6 * gd / vr, latency, 1.0e6 * latency / kHostRate,
                    (gd - 1.0) / 2.0) && ok;
    }
    return ok;
}

// ===========================================================================

bool smokeAliasAB()
{
    vibesynth::Patch patch;
    patch.name = "alias probe";
    patch.voice = "out = fx_fold(osc_sine(0, freq), 1) * 0.25;";
    const auto p1 = buildProgram (patch, 1);
    const auto p2 = buildProgram (patch, 2);
    if (p1 == nullptr || p2 == nullptr)
        return false;

    // A stepped pitch sweep: notes 60..108 every 4 semitones, each held
    // 0.5 s; the last 16384 samples of each note are analysed.
    constexpr size_t kFft = 16384;
    const int hold = (int) (0.5 * kHostRate);
    double sum1 = 0.0, sum2 = 0.0, worst1 = -300.0, worst2 = -300.0;
    int notes = 0;
    for (int note = 60; note <= 108; note += 4, ++notes)
    {
        Script s;
        s.on (0, note);
        PatchPlayer a, b;
        const auto x1 = play (a, *p1, s, hold, 512, Mode::Poly, 0.0);
        const auto x2 = play (b, *p2, s, hold, 512, Mode::Poly, 0.0);
        const double f0 = PatchPlayer::noteHz (note);
        const double i1 = inharmonicDb (x1, (size_t) hold - kFft, kFft, f0, kHostRate);
        const double i2 = inharmonicDb (x2, (size_t) hold - kFft, kFft, f0, kHostRate);
        sum1 += i1;
        sum2 += i2;
        worst1 = std::max (worst1, i1);
        worst2 = std::max (worst2, i2);
    }
    const double mean1 = sum1 / notes, mean2 = sum2 / notes;
    return check (mean2 <= mean1 - 10.0,
                  "alias A/B, fx_fold(osc_sine, 1) over notes 60-108: inharmonic energy mean %.1f dB "
                  "at --os 1, %.1f dB at --os 2 (%.1f dB better; worst note %.1f vs %.1f dB; want >= 10 dB)",
                  mean1, mean2, mean1 - mean2, worst1, worst2);
}

// ===========================================================================

bool smokeFilters()
{
    using vibesynth::FilterRate;
    using vibesynth::LadderState;
    using vibesynth::SvfState;
    bool ok = true;

    // Prewarp accuracy over the whole map at both rates.
    for (const double fs : { kHostRate, 2.0 * kHostRate })
    {
        FilterRate fr;
        fr.init (fs);
        double worst = 0.0;
        for (int i = 0; i <= 100000; ++i)
        {
            const double c = i / 100000.0;
            const double hz = std::min (normToHz (c), 0.45 * fs);
            const double approx = std::atan (fr.g (c)) * fs / kPi;
            worst = std::max (worst, std::fabs (cents (approx, hz)));
        }
        // Worst near 20 kHz at 48 kHz (the master's rate), where tan bends
        // most between table points.
        ok = check (worst < 1.0, "filter cutoff map at %.0f kHz: max error %.4f cents vs exact tan "
                    "(513-point table; want < 1)", fs / 1000.0, worst) && ok;
    }

    const double fs = 2.0 * kHostRate; // the voice rate
    FilterRate fr;
    fr.init (fs);

    // SVF LP cutoff: the -90 degree point, res 0, at 3 points.
    for (const double c : { 0.2, 0.5, 0.8 })
    {
        const double want = normToHz (c);
        const double got = bisectHz (want / 8.0, want * 8.0, [&] (double f)
        {
            SvfState s;
            const auto g = probe ([&] (float x) { return s.process (x, c, 0.0, fr, SvfState::Low); }, f, fs);
            return std::arg (g) > -0.5 * kPi;
        });
        const double err = cents (got, want) / 100.0;
        ok = check (std::fabs (err) <= 1.0, "flt_lp cutoff %.1f: %.1f Hz measured, %.1f Hz expected "
                    "(%+.3f semitones, want +-1)", c, got, want, err) && ok;
    }

    // Resonance peak: LP at res 0.9 and 1.0, cutoff 0.5.
    for (const double res : { 0.9, 1.0 })
    {
        const double fc = normToHz (0.5);
        double peak = 0.0, peakHz = 0.0;
        for (int i = -48; i <= 48; ++i)
        {
            const double f = fc * std::pow (2.0, i / 96.0);
            SvfState s;
            const double g = std::abs (probe ([&] (float x) { return s.process (x, 0.5, res, fr, SvfState::Low); },
                                              f, fs));
            if (g > peak) { peak = g; peakHz = f; }
        }
        const double k = std::max (vibesynth::kSvfMinDamping, 2.0 * (1.0 - std::sqrt (std::sqrt (res))));
        ok = check (finite (peak) && std::fabs (cents (peakHz, fc)) <= 100.0 && db (peak) >= 20.0
                        && std::fabs (db (peak) - db (1.0 / k)) <= 1.0,
                    "flt_lp resonance %.1f: peak %+.1f dB at %.0f Hz (cutoff %.0f Hz; want >= +20 dB within "
                    "1 semitone, Q = %.1f -> %+.1f dB)", res, db (peak), peakHz, fc, 1.0 / k, db (1.0 / k)) && ok;
    }

    // HP and BP shapes around cutoff 0.5.
    {
        const double fc = normToHz (0.5);
        auto gainAt = [&] (int tap, double res, double f)
        {
            SvfState s;
            return std::abs (probe ([&] (float x) { return s.process (x, 0.5, res, fr, tap); }, f, fs));
        };
        const double lo = db (gainAt (SvfState::High, 0.0, fc / 4.0));
        const double hi = db (gainAt (SvfState::High, 0.0, fc * 4.0));
        ok = check (lo <= -20.0 && lo >= -30.0 && hi >= -1.0 && hi <= 0.5,
                    "flt_hp res 0: %.1f dB two octaves below cutoff, %.1f dB two octaves above "
                    "(want -20..-30, -1..+0.5)", lo, hi) && ok;
        const double centre = db (gainAt (SvfState::Band, 0.5, fc));
        const double below = db (gainAt (SvfState::Band, 0.5, fc / 4.0));
        const double above = db (gainAt (SvfState::Band, 0.5, fc * 4.0));
        double bpPeakHz = 0.0, bpPeak = 0.0;
        for (int i = -24; i <= 24; ++i)
        {
            const double f = fc * std::pow (2.0, i / 48.0);
            const double g = gainAt (SvfState::Band, 0.5, f);
            if (g > bpPeak) { bpPeak = g; bpPeakHz = f; }
        }
        ok = check (centre - below >= 12.0 && centre - above >= 12.0
                        && std::fabs (cents (bpPeakHz, fc)) <= 100.0,
                    "flt_bp res 0.5: peak at %.0f Hz (cutoff %.0f), %+.1f dB at cutoff, %+.1f / %+.1f dB "
                    "two octaves away (want within 1 semitone, >= 12 dB down)", bpPeakHz, fc, centre,
                    below, above) && ok;
    }

    // Ladder cutoff at res 0: |H| = 0.25 (-12 dB, four poles at -3 dB).
    for (const double c : { 0.2, 0.5, 0.8 })
    {
        const double want = normToHz (c);
        const double got = bisectHz (want / 8.0, want * 8.0, [&] (double f)
        {
            LadderState l;
            return std::abs (probe ([&] (float x) { return l.process (x, c, 0.0, fr); }, f, fs, 0.01)) > 0.25;
        });
        const double err = cents (got, want) / 100.0;
        ok = check (std::fabs (err) <= 1.0, "flt_ladder cutoff %.1f: %.1f Hz measured, %.1f Hz expected "
                    "(%+.3f semitones, want +-1)", c, got, want, err) && ok;
    }
    {
        const double fc = normToHz (0.5);
        LadderState l;
        const double slope = db (std::abs (probe ([&] (float x) { return l.process (x, 0.5, 0.0, fr); },
                                                  4.0 * fc, fs, 0.01)));
        double peak = 0.0, peakHz = 0.0;
        for (int i = -24; i <= 24; ++i)
        {
            const double f = fc * std::pow (2.0, i / 48.0);
            LadderState r;
            const double g = std::abs (probe ([&] (float x) { return r.process (x, 0.5, 0.8, fr); }, f, fs, 0.01));
            if (g > peak) { peak = g; peakHz = f; }
        }
        ok = check (slope <= -40.0 && std::fabs (cents (peakHz, fc)) <= 100.0,
                    "flt_ladder shape: %.1f dB two octaves above cutoff (want <= -40, 24 dB/oct); "
                    "res 0.8 peaks at %.0f Hz (cutoff %.0f, want within 1 semitone), %+.1f dB",
                    slope, peakHz, fc, db (peak)) && ok;
    }

    // Ladder self-oscillation: an impulse at res 1 rings on, bounded and
    // in tune; at res 0.9 it dies away.
    for (const double res : { 1.0, 0.9 })
    {
        LadderState l;
        const int total = (int) (1.5 * fs);
        std::vector<float> y ((size_t) total);
        double peak = 0.0;
        for (int i = 0; i < total; ++i)
        {
            y[(size_t) i] = l.process (i == 0 ? 0.5f : 0.0f, 0.5, res, fr);
            peak = std::max (peak, std::fabs ((double) y[(size_t) i]));
        }
        double sum = 0.0;
        int crossings = 0;
        const int from = total - (int) (0.5 * fs);
        for (int i = from; i < total; ++i)
        {
            sum += (double) y[(size_t) i] * y[(size_t) i];
            if (y[(size_t) i - 1] <= 0.0f && y[(size_t) i] > 0.0f)
                ++crossings;
        }
        const double rms = std::sqrt (sum / (total - from));
        if (res == 1.0)
        {
            const double hz = crossings / 0.5;
            ok = check (finite (peak) && rms > 0.05 && peak < 2.0
                            && std::fabs (cents (hz, normToHz (0.5))) <= 100.0,
                        "flt_ladder res 1 self-oscillates: RMS %.3f, peak %.3f, %.0f Hz (cutoff %.0f Hz; "
                        "want RMS > 0.05, peak < 2, within 1 semitone)", rms, peak, hz, normToHz (0.5)) && ok;
        }
        else
        {
            ok = check (rms < 1.0e-4, "flt_ladder res 0.9 rings out: RMS %.2e after 1 s (want < 1e-4)",
                        rms) && ok;
        }
    }

    // No NaN, bounded: +12 dB saw, cutoff swept 0 -> 1 -> 0 in 1 ms ramps
    // (with 1 ms holds), res 0 / 0.5 / 1, every filter; plus NaN inputs.
    {
        const int ramp = (int) (0.001 * fs);
        double worstPeak = 0.0;
        bool allFinite = true;
        for (int which = 0; which < 4; ++which)
        {
            for (const double res : { 0.0, 0.5, 1.0 })
            {
                SvfState s;
                LadderState l;
                double phase = 0.0;
                for (int i = 0; i < 40 * 4 * ramp; ++i)
                {
                    const int seg = (i / ramp) % 4;
                    const double t = static_cast<double> (i % ramp) / ramp;
                    const double c = seg == 0 ? t : seg == 1 ? 1.0 : seg == 2 ? 1.0 - t : 0.0;
                    phase += 110.0 / fs;
                    phase -= std::floor (phase);
                    const float x = static_cast<float> (4.0 * (2.0 * phase - 1.0));
                    const float y = which == 3 ? l.process (x, c, res, fr) : s.process (x, c, res, fr, which);
                    allFinite = allFinite && finite (y);
                    worstPeak = std::max (worstPeak, std::fabs ((double) y));
                }
                const double nan = std::nan ("");
                const float y = which == 3 ? l.process (1.0f, nan, nan, fr) : s.process (1.0f, nan, nan, fr, which);
                allFinite = allFinite && finite (y);
            }
        }
        ok = check (allFinite && worstPeak < 100.0,
                    "flt_lp/hp/bp/ladder: +12 dB saw, cutoff 0 -> 1 -> 0 in 1 ms ramps, res 0/0.5/1, and NaN "
                    "cutoff/res: all finite, peak %.1f (want < 100)", worstPeak) && ok;
    }

    // Zipper (at 48 kHz, where the DaisySP Svf stepped every 16 samples): a
    // 200 Hz sine through flt_lp with the cutoff swept by a 20 Hz LFO. A
    // per-sample sweep leaves nothing near fs/16 = 3 kHz; the reference
    // feeds the same LFO stepped every 16 samples, which shows the comb.
    {
        const char* smooth = "lfo = 0.45 + 0.25 * osc_sine(2, 20); out = flt_lp(0, osc_sine(1, 200), lfo, 0);";
        const char* stepped = "lfo = 0.45 + 0.25 * osc_sine(2, 20); (n % 16) == 0 ? (c = lfo); n += 1; "
                              "out = flt_lp(0, osc_sine(1, 200), c, 0);";
        double hf[2] = {}, line[2] = {};
        for (int k = 0; k < 2; ++k)
        {
            const vibesynth::PatchStrings ps = { "0;", k == 0 ? smooth : stepped, "outl = inl; outr = inr;" };
            const auto v = std::make_unique<vibesynth::EelVoice> (ps, kHostRate, 3u);
            if (! v->isValid())
                return check (false, "zipper probe compiles: %s", v->compileError().c_str());
            double macros[vibesynth::MacroSmoother::kNumMacros] = {};
            std::vector<float> x ((size_t) kHostRate);
            for (size_t i = 0; i < x.size(); ++i)
                x[i] = v->renderSample (200.0, 1.0, 55.0, 0.9, macros);
            constexpr size_t n = 32768;
            const std::vector<double> p = powerSpectrum (x, x.size() - n, n);
            const double binHz = kHostRate / n;
            double total = 0.0, high = 0.0, comb = 0.0, carrier = 0.0;
            for (size_t b = 1; b < p.size(); ++b)
            {
                const double f = b * binHz;
                total += p[b];
                if (f >= 1500.0)
                    high += p[b];
                if (std::fabs (f - 3000.0) <= 400.0)
                    comb = std::max (comb, p[b]);
                if (std::fabs (f - 200.0) <= 40.0)
                    carrier = std::max (carrier, p[b]);
            }
            hf[k] = 10.0 * std::log10 (std::max (high, 1e-300) / total);
            line[k] = 10.0 * std::log10 (std::max (comb, 1e-300) / carrier);
        }
        ok = check (hf[0] <= -90.0 && hf[1] >= hf[0] + 20.0,
                    "zipper: fast cutoff sweep on a 200 Hz sine, energy above 1.5 kHz %.1f dB per-sample vs "
                    "%.1f dB with 16-sample steps; strongest line near fs/16 (3 kHz) %.1f vs %.1f dB re carrier "
                    "(want <= -90 dB and the stepped reference >= 20 dB worse)",
                    hf[0], hf[1], line[0], line[1]) && ok;
    }
    return ok;
}

// ===========================================================================

bool smokeVoiceModes()
{
    bool ok = true;
    vibesynth::Patch probe;
    probe.name = "mode probe";
    probe.voiceMode = "poly";
    probe.init = "inits += 1;";
    probe.voice = "trigs += trig; f = freq; g = gate; w = mw; out = 0.1;";
    probe.master = "mwm = mw; outl = inl; outr = inr;";
    vibesynth::Patch monoProbe = probe;
    monoProbe.voiceMode = "mono";
    const auto poly = buildProgram (probe, 2);
    const auto mono = buildProgram (monoProbe, 2);
    if (poly == nullptr || mono == nullptr)
        return false;
    const int ms = (int) (kHostRate / 1000.0);

    // Legato, return to the held note, retrigger in place.
    {
        Script s;
        s.on (0, 40);
        s.on (100 * ms, 47);     // legato up
        s.off (500 * ms, 47);    // back to 40, still held
        s.off (1000 * ms, 40);   // all keys up
        s.on (1100 * ms, 45);    // retrigger in place
        PatchPlayer player;
        int slot0 = -1;
        bool gateHeld = true, oneVoice = true;
        double inits1 = -1, trigs1 = -1, note1 = -1, inits2 = -1, trigs2 = -1, note2 = -1, freq2 = 0;
        double inits3 = -1, trigs3 = -1, freq3 = 0;
        int slot3 = -1, sounding3 = 0;
        play (player, *mono, s, 1200 * ms, 32, Mode::Patch, 0.3, [&] (int end)
        {
            const int slot = firstSlot (player);
            if (end <= 32)
                slot0 = slot;
            if (end < 1000 * ms)
            {
                gateHeld = gateHeld && slot >= 0 && voiceVar (*mono, player, slot, "g") == 1.0;
                oneVoice = oneVoice && player.soundingVoices() == 1 && slot == slot0;
            }
            if (end == 400 * ms)
            {
                inits1 = voiceVar (*mono, player, slot, "inits");
                trigs1 = voiceVar (*mono, player, slot, "trigs");
                note1 = player.slotNote (slot);
            }
            if (end == 960 * ms)
            {
                inits2 = voiceVar (*mono, player, slot, "inits");
                trigs2 = voiceVar (*mono, player, slot, "trigs");
                note2 = player.slotNote (slot);
                freq2 = player.slotFreq (slot);
            }
            if (end == 1100 * ms + 32)
            {
                slot3 = slot;
                sounding3 = player.soundingVoices();
                inits3 = voiceVar (*mono, player, slot, "inits");
                trigs3 = voiceVar (*mono, player, slot, "trigs");
                freq3 = player.slotFreq (slot);
            }
        });
        ok = check (gateHeld && oneVoice && inits1 == 1.0 && trigs1 == 1.0 && note1 == 47.0,
                    "mono legato 40 -> 47 (overlapping): one voice, gate held throughout, init ran %.0fx, "
                    "trig %.0fx (want 1, 1), note %.0f", inits1, trigs1, note1) && ok;
        ok = check (inits2 == 1.0 && trigs2 == 1.0 && note2 == 40.0
                        && std::fabs (cents (freq2, PatchPlayer::noteHz (40))) < 0.01,
                    "mono release of the top note returns to the held 40: note %.0f, %.2f Hz, still init %.0fx, "
                    "trig %.0fx", note2, freq2, inits2, trigs2) && ok;
        ok = check (slot3 == slot0 && sounding3 == 1 && inits3 == 2.0 && trigs3 == 2.0
                        && std::fabs (cents (freq3, PatchPlayer::noteHz (45))) < 0.01,
                    "mono note after all keys up retriggers in place: slot %d (was %d), %d voice sounding, "
                    "init %.0fx, trig %.0fx, %.2f Hz without glide", slot3, slot0, sounding3, inits3, trigs3,
                    freq3) && ok;
    }

    // Glide time at Glide 0.5 (glide^2 = 0.25 s), linear in pitch.
    {
        Script s;
        s.on (0, 40);
        s.on (100 * ms, 52);
        PatchPlayer player;
        const double target = PatchPlayer::noteHz (52);
        int reachedAt = -1;
        double midPitch = 0.0;
        play (player, *mono, s, 600 * ms, 8, Mode::Mono, 0.5, [&] (int end)
        {
            const int slot = firstSlot (player);
            const double f = player.slotFreq (slot);
            if (end == 100 * ms + 125 * ms)
                midPitch = 69.0 + 12.0 * std::log2 (f / 440.0);
            if (reachedAt < 0 && end > 100 * ms && std::fabs (cents (f, target)) < 1.0)
                reachedAt = end;
        });
        const double seconds = (reachedAt - 100 * ms) / kHostRate;
        ok = check (std::fabs (seconds - 0.25) <= 0.025 && std::fabs (midPitch - 46.0) <= 0.25,
                    "glide 0.5 over an octave: reaches the target in %.3f s (want 0.25 +-10%%), halfway "
                    "pitch %.2f (want 46, linear in pitch)", seconds, midPitch) && ok;
    }

    // Pitch bend: full range ±2 semitones, smoothed ~5 ms, after glide.
    {
        Script s;
        s.on (0, 69);
        s.add (100 * ms, 0xE0, 0x7F, 0x7F);  // max
        s.add (200 * ms, 0xE0, 0x00, 0x00);  // min
        s.add (300 * ms, 0xE0, 0x00, 0x40);  // centre
        PatchPlayer player;
        double at5 = 0, up = 0, down = 0, centre = 0;
        play (player, *poly, s, 400 * ms, 16, Mode::Patch, 0.0, [&] (int end)
        {
            const int slot = firstSlot (player);
            const double f = voiceVar (*poly, player, slot, "f");
            if (end == 105 * ms) at5 = f;
            if (end == 160 * ms) up = f;
            if (end == 260 * ms) down = f;
            if (end == 360 * ms) centre = f;
        });
        const double maxSemis = 2.0 * 8191.0 / 8192.0;
        const double frac5 = cents (at5, 440.0) / (100.0 * maxSemis);
        ok = check (std::fabs (cents (up, 440.0) - 100.0 * maxSemis) < 0.1
                        && std::fabs (cents (down, 440.0) + 200.0) < 0.1
                        && std::fabs (cents (centre, 440.0)) < 0.1 && frac5 > 0.5 && frac5 < 0.75,
                    "pitch bend on A4: max %.2f Hz (%+.2f st), min %.2f Hz (%+.2f st), centre %.2f Hz; "
                    "%.0f%% of the way 5 ms after the bend (one-pole ~5 ms)", up, cents (up, 440.0) / 100.0,
                    down, cents (down, 440.0) / 100.0, centre, 100.0 * frac5) && ok;
    }

    // Mod wheel: CC1 127, smoothed ~10 ms, in the voice and the master.
    {
        Script s;
        s.on (0, 60);
        s.add (10 * ms, 0xB0, 1, 127);
        PatchPlayer player;
        double v10 = -1, v100 = -1, m100 = -1;
        play (player, *poly, s, 120 * ms, 16, Mode::Patch, 0.0, [&] (int end)
        {
            const int slot = firstSlot (player);
            if (end == 20 * ms) v10 = voiceVar (*poly, player, slot, "w");
            if (end == 110 * ms)
            {
                v100 = voiceVar (*poly, player, slot, "w");
                const EEL_F* m = poly->master().variable ("mwm");
                m100 = m != nullptr ? *m : -1.0;
            }
        });
        ok = check (v10 > 0.5 && v10 < 0.75 && v100 > 0.999 && m100 > 0.999,
                    "mod wheel CC1 127: mw %.3f after 10 ms (one-pole ~10 ms), %.4f after 100 ms in the voice, "
                    "%.4f in the master", v10, v100, m100) && ok;
    }

    // CC 123 in mono: the held stack clears, the next note retriggers.
    {
        Script s;
        s.on (0, 40);
        s.on (50 * ms, 43);
        s.add (100 * ms, 0xB0, 123, 0);
        s.on (150 * ms, 47);
        PatchPlayer player;
        double gateAfter = -1, inits = -1;
        play (player, *mono, s, 200 * ms, 16, Mode::Patch, 0.0, [&] (int end)
        {
            const int slot = firstSlot (player);
            if (end == 120 * ms) gateAfter = voiceVar (*mono, player, slot, "g");
            if (end == 180 * ms) inits = voiceVar (*mono, player, slot, "inits");
        });
        ok = check (gateAfter == 0.0 && inits == 2.0,
                    "mono CC 123: gate %.0f after it (want 0), the next note retriggers (init %.0fx, want 2)",
                    gateAfter, inits) && ok;
    }

    // Resolution: a 3-note chord under each mode setting.
    {
        struct Case { const char* what; PatchProgram* prog; Mode mode; int want; };
        const Case cases[] = {
            { "Patch on a mono patch", mono.get(), Mode::Patch, 1 },
            { "Patch on a poly patch", poly.get(), Mode::Patch, 3 },
            { "Poly on a mono patch", mono.get(), Mode::Poly, 3 },
            { "Mono on a poly patch", poly.get(), Mode::Mono, 1 },
        };
        std::string line;
        bool all = true;
        for (const Case& c : cases)
        {
            Script s;
            s.on (0, 40);
            s.on (0, 44);
            s.on (0, 47);
            PatchPlayer player;
            int sounding = 0, note = -1;
            play (player, *c.prog, s, 50 * ms, 32, c.mode, 0.0, [&] (int end)
            {
                if (end == 48 * ms)
                {
                    sounding = player.soundingVoices();
                    note = player.slotNote (firstSlot (player));
                }
            });
            const bool good = sounding == c.want && (c.want != 1 || note == 47);
            all = all && good;
            char buf[96];
            std::snprintf (buf, sizeof (buf), "%s%s -> %d", line.empty() ? "" : "; ", c.what, sounding);
            line += buf;
        }
        ok = check (all, "voice mode resolution, 3-note chord: %s (mono plays the last note)",
                    line.c_str()) && ok;
    }
    return ok;
}

} // namespace vibesynth_cli
