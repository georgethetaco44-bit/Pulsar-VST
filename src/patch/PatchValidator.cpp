// PatchProgram build + the SPEC §7.1 offline test matrix. Worker thread
// only (or the CLI). No fast-math: the NaN/Inf checks are bit-pattern
// tests (SPEC §11).

#include "patch/PatchValidator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <iterator>
#include <vector>

#include "engine/Oversampler.h"
#include "patch/LexiconLint.h"
#include "patch/LoopNesting.h"

namespace vibesynth {

std::string formatCompileError (const std::string& section, const std::string& text)
{
    if (section.empty())
        return "compile error: " + text;
    return "compile error in " + section + ": " + text;
}

std::unique_ptr<PatchProgram> PatchProgram::build (const Patch& patch, double sampleRate,
                                                   std::string& error, int oversampling)
{
    // Restores and built-ins skip the matrix, so the nesting check runs
    // here too: a 3-deep loop would stall the audio thread.
    const LoopNestingResult nesting = checkLoopNesting (patch);
    if (! nesting.ok())
    {
        error = loopNestingError (nesting);
        return nullptr;
    }

    std::unique_ptr<PatchProgram> prog (new PatchProgram());
    prog->patch_ = patch;
    prog->sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    prog->oversampling_ = std::clamp (oversampling, 1, kMaxOversampling);
    const double t = patch.trim;
    prog->trim_ = static_cast<float> (isFiniteBits (t) && t > 0.0 && t <= 1.0 ? t : 1.0);

    const PatchStrings ps = { prog->patch_.init.c_str(), prog->patch_.voice.c_str(),
                              prog->patch_.master.c_str() };

    prog->master_ = std::make_unique<EelMaster> (ps, prog->sampleRate_);
    if (! prog->master_->isValid())
    {
        error = formatCompileError ("master", prog->master_->compileError());
        return nullptr;
    }
    for (int i = 0; i < kNumVoices; ++i)
    {
        prog->voices_[i] = std::make_unique<EelVoice> (ps, prog->voiceRate(), voiceSeed (i));
        if (! prog->voices_[i]->isValid())
        {
            error = formatCompileError (prog->voices_[i]->compileSection(),
                                       prog->voices_[i]->compileError());
            return nullptr;
        }
    }
    prog->isStereo_ = prog->voices_[0]->isStereo();
    error.clear();
    return prog;
}

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kNotes[] = { 28, 40, 55 };
constexpr int kNumMacroSets = 3;          // all 0, defaults, all 1
constexpr double kConfigSeconds = 1.0;
constexpr double kNoteOffSeconds = 0.7;
constexpr double kDcWindowSeconds = 0.5;  // the last 0.5 s
constexpr int kChunk = 64;                // host samples per timing chunk
constexpr double kMaxConfigCpu = 0.50;    // voice + master, per configuration
constexpr double kDeadlineMs = 1000.0 * kConfigSeconds * kMaxConfigCpu; // 500 ms
constexpr double kMaxPeak = 3.9810717055349722;  // +12 dBFS
constexpr double kSilence = 1.0e-4;              // -80 dBFS
constexpr double kMaxDc = 0.1;
constexpr double kTrimTarget = 0.7079457843841379; // -3 dBFS

const char* macroSetName (int set)
{
    return set == 0 ? "macros all 0" : set == 1 ? "macros at defaults" : "macros all 1";
}

std::string where (int note, int set)
{
    return "note " + std::to_string (note) + ", " + macroSetName (set);
}

std::string fmt (const char* f, double v)
{
    char buf[32];
    std::snprintf (buf, sizeof (buf), f, v);
    return buf;
}

double msSince (Clock::time_point t0, Clock::time_point t1)
{
    return std::chrono::duration<double, std::milli> (t1 - t0).count();
}

ValidationResult reject (ValidationResult r, PatchStatus status, std::string message)
{
    r.status = status;
    r.error = std::move (message);
    return r;
}

std::atomic<double> cpuScaleSetting { 1.0 };

} // namespace

void setValidatorCpuScale (double scale) noexcept
{
    cpuScaleSetting.store (scale >= 1.0 ? scale : 1.0, std::memory_order_relaxed);
}

double validatorCpuScale() noexcept
{
    return cpuScaleSetting.load (std::memory_order_relaxed);
}

ValidationResult validatePatch (const Patch& patch, double sampleRate,
                                const std::atomic<bool>* cancel, int oversampling,
                                bool checkMacroNoise)
{
    ValidationResult result;
    const double deadlineMs = kDeadlineMs * validatorCpuScale();
    const auto wallStart = Clock::now();
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int os = std::clamp (oversampling, 1, PatchProgram::kMaxOversampling);
    const double voiceRate = sr * os;
    const int total = static_cast<int> (std::lround (sr * kConfigSeconds));
    const int noteOff = static_cast<int> (std::lround (sr * kNoteOffSeconds));
    const int dcStart = total - static_cast<int> (std::lround (sr * kDcWindowSeconds));

    const PatchStrings ps = { patch.init.c_str(), patch.voice.c_str(), patch.master.c_str() };
    float voiceBuf[kChunk * PatchProgram::kMaxOversampling];
    float hostBuf[kChunk];
    double voiceNs = 0.0;
    double worstPeak = 0.0;
    auto finish = [&] (ValidationResult r)
    {
        r.peak = worstPeak;
        r.wallMs = msSince (wallStart, Clock::now());
        return r;
    };

    // M8 (SPEC §6.4): unknown functions, argument counts and literal slots,
    // with suggestions the self-heal retry can act on.
    const LintResult lint = lintPatch (patch);
    if (! lint.ok())
        return finish (reject (result, PatchStatus::LintError, lint.message()));

    // Before compiling (SPEC §7.1): with the 1024 loop cap, more than two
    // levels could make one sample run for seconds, past any deadline.
    const LoopNestingResult nesting = checkLoopNesting (patch);
    if (! nesting.ok())
        return finish (reject (result, PatchStatus::NestedLoops, loopNestingError (nesting)));

    int config = 0;
    for (const int note : kNotes)
    {
        const double freq = 440.0 * std::pow (2.0, (note - 69) / 12.0);
        for (int set = 0; set < kNumMacroSets; ++set)
        {
            double macros[MacroSmoother::kNumMacros];
            for (int m = 0; m < MacroSmoother::kNumMacros; ++m)
            {
                const double raw = set == 0 ? 0.0 : set == 2 ? 1.0 : patch.macroDefault (m);
                macros[m] = patch.applyMacroRange (m, raw);
            }

            // Fresh test VMs for every configuration (SPEC §7.1); the
            // voice at the oversampled rate, as the plugin runs it.
            const auto voice = std::make_unique<EelVoice> (ps, voiceRate, PatchProgram::voiceSeed (0));
            if (! voice->isValid())
                return finish (reject (result, PatchStatus::CompileError,
                                       formatCompileError (voice->compileSection(),
                                                           voice->compileError())));
            const auto master = std::make_unique<EelMaster> (ps, sr);
            if (! master->isValid())
                return finish (reject (result, PatchStatus::CompileError,
                                       formatCompileError ("master", master->compileError())));

            HalfbandDecimator decimator;
            HalfbandDecimator decimatorR;
            const bool stereo = voice->isStereo();
            float voiceBufR[kChunk * PatchProgram::kMaxOversampling];
            float hostBufR[kChunk];
            double peak = 0.0;
            double sumL = 0.0, sumR = 0.0;
            // No host: 120 bpm, beat 0 at the note-on (SPEC §4.3).
            Transport transport;
            const double beatStep = transport.beatStep (sr);
            const double voiceBeatStep = transport.beatStep (voiceRate);
            const auto configStart = Clock::now();
            voice->noteOn (freq, note, 1.0, transport);

            for (int start = 0; start < total; start += kChunk)
            {
                const int n = std::min (kChunk, total - start);
                const int vn = n * os;
                const int vstart = start * os;
                const auto t0 = Clock::now();
                auto t1 = t0;
                if (! stereo)
                {
                    for (int k = 0; k < vn; ++k)
                    {
                        const double gate = start + k / os < noteOff ? 1.0 : 0.0;
                        transport.beat = (vstart + k) * voiceBeatStep;
                        voiceBuf[k] = voice->renderSample (freq, gate, note, 1.0, macros, transport);

                        // Runaway guard (SPEC §7.1), after every sample: one
                        // sample is at most ~1M loop iterations (nesting check
                        // + 1024 cap), so a slow patch stops within a few ms
                        // of the deadline and the worker can always be joined.
                        t1 = Clock::now();
                        if (msSince (configStart, t1) > deadlineMs)
                            return finish (reject (result, PatchStatus::TooSlow,
                                                   "too slow: over " + fmt ("%.0f", deadlineMs)
                                                       + " ms to render 1 s (" + where (note, set) + ")"));
                        if (cancel != nullptr && cancel->load (std::memory_order_relaxed))
                        {
                            result.cancelled = true;
                            return finish (reject (result, PatchStatus::TooSlow, "cancelled"));
                        }
                    }
                }
                else
                {
                    for (int k = 0; k < vn; ++k)
                    {
                        const double gate = start + k / os < noteOff ? 1.0 : 0.0;
                        transport.beat = (vstart + k) * voiceBeatStep;
                        voice->renderSample (freq, gate, note, 1.0, macros, transport, 0.0,
                                             voiceBuf[k], voiceBufR[k]);

                        t1 = Clock::now();
                        if (msSince (configStart, t1) > deadlineMs)
                            return finish (reject (result, PatchStatus::TooSlow,
                                                   "too slow: over " + fmt ("%.0f", deadlineMs)
                                                       + " ms to render 1 s (" + where (note, set) + ")"));
                        if (cancel != nullptr && cancel->load (std::memory_order_relaxed))
                        {
                            result.cancelled = true;
                            return finish (reject (result, PatchStatus::TooSlow, "cancelled"));
                        }
                    }
                }
                // Includes the per-sample clock reads (~20 ns, ~0.1 % CPU).
                voiceNs += std::chrono::duration<double, std::nano> (t1 - t0).count();

                if (! stereo)
                {
                    for (int k = 0; k < vn; ++k)
                        if (! isFiniteBits (voiceBuf[k]))
                            return finish (reject (result, PatchStatus::NonFinite,
                                                   "NaN or Inf in voice output at " + where (note, set)
                                                       + ", after " + fmt ("%.2f s", (vstart + k) / voiceRate)));
                    if (os == 2)
                        for (int k = 0; k < n; ++k)
                            hostBuf[k] = decimator.process (voiceBuf[2 * k], voiceBuf[2 * k + 1]);
                    else
                        std::copy (voiceBuf, voiceBuf + n, hostBuf);

                    for (int k = 0; k < n; ++k)
                    {
                        const int i = start + k;
                        const float v = hostBuf[k];
                        float l = 0.0f, r = 0.0f;
                        transport.beat = i * beatStep;
                        master->renderSample (v, v, l, r, macros, transport);
                        if (! isFiniteBits (l) || ! isFiniteBits (r))
                            return finish (reject (result, PatchStatus::NonFinite,
                                                   "NaN or Inf in master output at " + where (note, set)
                                                       + ", after " + fmt ("%.2f s", i / sr)));
                        const double al = std::fabs (static_cast<double> (l));
                        const double ar = std::fabs (static_cast<double> (r));
                        peak = std::max (peak, std::max (al, ar));
                        if (i >= dcStart)
                        {
                            sumL += l;
                            sumR += r;
                        }
                    }
                }
                else
                {
                    for (int k = 0; k < vn; ++k)
                        if (! isFiniteBits (voiceBuf[k]) || ! isFiniteBits (voiceBufR[k]))
                            return finish (reject (result, PatchStatus::NonFinite,
                                                   "NaN or Inf in voice output at " + where (note, set)
                                                       + ", after " + fmt ("%.2f s", (vstart + k) / voiceRate)));
                    if (os == 2)
                    {
                        for (int k = 0; k < n; ++k)
                        {
                            hostBuf[k] = decimator.process (voiceBuf[2 * k], voiceBuf[2 * k + 1]);
                            hostBufR[k] = decimatorR.process (voiceBufR[2 * k], voiceBufR[2 * k + 1]);
                        }
                    }
                    else
                    {
                        std::copy (voiceBuf, voiceBuf + n, hostBuf);
                        std::copy (voiceBufR, voiceBufR + n, hostBufR);
                    }

                    for (int k = 0; k < n; ++k)
                    {
                        const int i = start + k;
                        const float vl = hostBuf[k];
                        const float vr = hostBufR[k];
                        float l = 0.0f, r = 0.0f;
                        transport.beat = i * beatStep;
                        master->renderSample (vl, vr, l, r, macros, transport);
                        if (! isFiniteBits (l) || ! isFiniteBits (r))
                            return finish (reject (result, PatchStatus::NonFinite,
                                                   "NaN or Inf in master output at " + where (note, set)
                                                       + ", after " + fmt ("%.2f s", i / sr)));
                        const double al = std::fabs (static_cast<double> (l));
                        const double ar = std::fabs (static_cast<double> (r));
                        peak = std::max (peak, std::max (al, ar));
                        if (i >= dcStart)
                        {
                            sumL += l;
                            sumR += r;
                        }
                    }
                }
            }

            // CPU rule (SPEC §7.1): the deadline above only looks during the
            // voice loop, so the last chunk's master is checked here.
            const double configMs = msSince (configStart, Clock::now());
            result.configCpu = std::max (result.configCpu, configMs / (1000.0 * kConfigSeconds));
            if (configMs > deadlineMs)
                return finish (reject (result, PatchStatus::TooSlow,
                                       "too slow: " + fmt ("%.0f", configMs) + " ms to render 1 s (max "
                                           + fmt ("%.0f", deadlineMs) + ", " + where (note, set) + ")"));

            // M9: the node list (SPEC §5.1 rule 4) is every touched slot.
            TouchedNodes::add (result.touched.voice, voice->rack().touched);
            TouchedNodes::add (result.touched.master, master->rack().touched);
            TouchedNodes::add (result.touched.master, master->voiceRack().touched);

            const double window = static_cast<double> (total - dcStart);
            const double dc = std::max (std::fabs (sumL), std::fabs (sumR)) / window;
            result.configDc[config] = dc;
            result.configPeak[config++] = peak;
            worstPeak = std::max (worstPeak, peak);
            if (peak > kMaxPeak)
                return finish (reject (result, PatchStatus::TooLoud,
                                       "too loud: peak " + fmt ("%+.1f", 20.0 * std::log10 (peak))
                                           + " dBFS (max +12) at " + where (note, set)));
            if (dc > kMaxDc)
                return finish (reject (result, PatchStatus::DcOffset,
                                       "DC offset " + fmt ("%.2f", dc) + " (max 0.1) at "
                                           + where (note, set)));
        }
    }

    if (worstPeak < kSilence)
        return finish (reject (result, PatchStatus::Silent, "patch makes no sound"));

    if (checkMacroNoise)
    {
        const auto scan = scanPatchMacros (patch, sr, os);
        for (const auto& entry : scan)
        {
            if (entry.isNoise)
            {
                return finish (reject (result, PatchStatus::MacroNoise,
                                       "m" + std::to_string (entry.macro) + " at 1 turns the sound into noise (flatness "
                                           + fmt ("%.2f", entry.at1Flatness) + " vs " + fmt ("%.2f", entry.defaultFlatness)
                                           + " at its default). Reduce its range so 1 is the most extreme setting a producer would use."));
            }
        }
    }

    const double audioNs = 1.0e9 * kConfigSeconds * static_cast<double> (std::size (kNotes) * kNumMacroSets);
    result.voiceCpu = voiceNs / audioNs;   // reported only

    result.trim = std::min (1.0, quantisePatchNumber (kTrimTarget / worstPeak));
    if (result.trim <= 0.0)
        result.trim = 1.0e-6;
    return finish (result);
}

TouchedNodes probeTouchedNodes (const Patch& patch, double sampleRate,
                                const std::atomic<bool>* cancel, int oversampling)
{
    constexpr double kProbeSeconds = 0.5;
    constexpr double kProbeNoteOffSeconds = 0.35;
    TouchedNodes touched;
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int os = std::clamp (oversampling, 1, PatchProgram::kMaxOversampling);
    const double voiceRate = sr * os;
    const int total = static_cast<int> (std::lround (sr * kProbeSeconds));
    const int noteOff = static_cast<int> (std::lround (sr * kProbeNoteOffSeconds));

    const PatchStrings ps = { patch.init.c_str(), patch.voice.c_str(), patch.master.c_str() };
    const auto voice = std::make_unique<EelVoice> (ps, voiceRate, PatchProgram::voiceSeed (0));
    const auto master = std::make_unique<EelMaster> (ps, sr);
    if (! voice->isValid() || ! master->isValid())
        return {};

    float voiceBuf[kChunk * PatchProgram::kMaxOversampling];
    for (const int note : kNotes)
    {
        const double freq = 440.0 * std::pow (2.0, (note - 69) / 12.0);
        for (int set = 0; set < kNumMacroSets; ++set)
        {
            double macros[MacroSmoother::kNumMacros];
            for (int m = 0; m < MacroSmoother::kNumMacros; ++m)
            {
                const double raw = set == 0 ? 0.0 : set == 2 ? 1.0 : patch.macroDefault (m);
                macros[m] = patch.applyMacroRange (m, raw);
            }

            // resetState() leaves the VMs as fresh as the validator's.
            voice->resetState();
            master->resetState();
            HalfbandDecimator decimator;
            HalfbandDecimator decimatorR;
            const bool stereo = voice->isStereo();
            float voiceBufR[kChunk * PatchProgram::kMaxOversampling];
            Transport transport;
            const double beatStep = transport.beatStep (sr);
            const double voiceBeatStep = transport.beatStep (voiceRate);
            voice->noteOn (freq, note, 1.0, transport);

            for (int start = 0; start < total; start += kChunk)
            {
                if (cancel != nullptr && cancel->load (std::memory_order_relaxed))
                    return {};
                const int n = std::min (kChunk, total - start);
                const int vstart = start * os;
                if (! stereo)
                {
                    for (int k = 0; k < n * os; ++k)
                    {
                        const double gate = start + k / os < noteOff ? 1.0 : 0.0;
                        transport.beat = (vstart + k) * voiceBeatStep;
                        voiceBuf[k] = voice->renderSample (freq, gate, note, 1.0, macros, transport);
                    }
                    for (int k = 0; k < n; ++k)
                    {
                        float v = voiceBuf[k];
                        if (os == 2)
                            v = decimator.process (voiceBuf[2 * k], voiceBuf[2 * k + 1]);
                        if (! isFiniteBits (v))
                            v = 0.0f;
                        float l = 0.0f, r = 0.0f;
                        transport.beat = (start + k) * beatStep;
                        master->renderSample (v, v, l, r, macros, transport);
                    }
                }
                else
                {
                    for (int k = 0; k < n * os; ++k)
                    {
                        const double gate = start + k / os < noteOff ? 1.0 : 0.0;
                        transport.beat = (vstart + k) * voiceBeatStep;
                        voice->renderSample (freq, gate, note, 1.0, macros, transport, 0.0,
                                             voiceBuf[k], voiceBufR[k]);
                    }
                    for (int k = 0; k < n; ++k)
                    {
                        float vl = voiceBuf[k];
                        float vr = voiceBufR[k];
                        if (os == 2)
                        {
                            vl = decimator.process (voiceBuf[2 * k], voiceBuf[2 * k + 1]);
                            vr = decimatorR.process (voiceBufR[2 * k], voiceBufR[2 * k + 1]);
                        }
                        if (! isFiniteBits (vl)) vl = 0.0f;
                        if (! isFiniteBits (vr)) vr = 0.0f;
                        float l = 0.0f, r = 0.0f;
                        transport.beat = (start + k) * beatStep;
                        master->renderSample (vl, vr, l, r, macros, transport);
                    }
                }
            }
            TouchedNodes::add (touched.voice, voice->rack().touched);
            TouchedNodes::add (touched.master, master->rack().touched);
            TouchedNodes::add (touched.master, master->voiceRack().touched);
        }
    }
    return touched;
}

double measureVoiceNs (const Patch& patch, double sampleRate, int runs, int samples, int oversampling)
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double voiceRate = sr * std::clamp (oversampling, 1, PatchProgram::kMaxOversampling);
    const PatchStrings ps = { patch.init.c_str(), patch.voice.c_str(), patch.master.c_str() };
    const auto voice = std::make_unique<EelVoice> (ps, voiceRate, PatchProgram::voiceSeed (0));
    if (! voice->isValid() || runs < 1 || samples < 1)
        return 0.0;
    double macros[MacroSmoother::kNumMacros];
    for (int m = 0; m < MacroSmoother::kNumMacros; ++m)
        macros[m] = patch.macroDefault (m);
    constexpr double kFreq = 55.0, kNote = 33.0;
    Transport transport;
    const double step = transport.beatStep (voiceRate);
    voice->noteOn (kFreq, kNote, 0.9, transport);
    volatile float sink = 0.0f;
    const bool stereo = voice->isStereo();
    std::vector<double> times;
    for (int r = -1; r < runs; ++r) // run -1: untimed warm-up
    {
        float acc = 0.0f;
        const auto t0 = Clock::now();
        if (! stereo)
        {
            for (int i = 0; i < samples; ++i)
            {
                transport.beat += step;
                acc += voice->renderSample (kFreq, 1.0, kNote, 0.9, macros, transport);
            }
        }
        else
        {
            float sL = 0.0f, sR = 0.0f;
            for (int i = 0; i < samples; ++i)
            {
                transport.beat += step;
                voice->renderSample (kFreq, 1.0, kNote, 0.9, macros, transport, 0.0, sL, sR);
                acc += sL + sR;
            }
        }
        const auto t1 = Clock::now();
        sink = sink + acc;
        if (r >= 0)
            times.push_back (std::chrono::duration<double, std::nano> (t1 - t0).count() / samples);
    }
    std::sort (times.begin(), times.end());
    return times[times.size() / 2];
}

namespace {

constexpr double kPi = 3.14159265358979323846;

void radix2Fft (std::vector<std::complex<double>>& a)
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

void renderMacroNote (const Patch& patch, int note, const double* rawMacros,
                      double sr, int os, std::vector<float>& outL, std::vector<float>& outR)
{
    const int total = static_cast<int> (sr);
    outL.assign (total, 0.0f);
    outR.assign (total, 0.0f);

    double macros[MacroSmoother::kNumMacros];
    for (int m = 0; m < MacroSmoother::kNumMacros; ++m)
        macros[m] = patch.applyMacroRange (m, rawMacros[m]);

    const double voiceRate = sr * os;
    const double freq = 440.0 * std::pow (2.0, (note - 69) / 12.0);
    const PatchStrings ps { patch.init.c_str(), patch.voice.c_str(), patch.master.c_str() };
    const auto voice = std::make_unique<EelVoice> (ps, voiceRate, PatchProgram::voiceSeed (0));
    if (! voice->isValid()) return;
    const auto master = std::make_unique<EelMaster> (ps, sr);
    if (! master->isValid()) return;

    HalfbandDecimator decimator;
    HalfbandDecimator decimatorR;
    const bool stereo = voice->isStereo();
    Transport transport;
    const double beatStep = transport.beatStep (sr);
    const double voiceBeatStep = transport.beatStep (voiceRate);
    voice->noteOn (freq, note, 1.0, transport);

    constexpr int kChunk = 512;
    float voiceBuf[kChunk * PatchProgram::kMaxOversampling];
    float voiceBufR[kChunk * PatchProgram::kMaxOversampling];
    float hostBuf[kChunk];
    float hostBufR[kChunk];

    for (int start = 0; start < total; start += kChunk)
    {
        const int n = std::min (kChunk, total - start);
        const int vn = n * os;
        const int vstart = start * os;
        if (! stereo)
        {
            for (int k = 0; k < vn; ++k)
            {
                transport.beat = (vstart + k) * voiceBeatStep;
                voiceBuf[k] = voice->renderSample (freq, 1.0, note, 1.0, macros, transport);
            }
            if (os == 2)
                for (int k = 0; k < n; ++k)
                    hostBuf[k] = decimator.process (voiceBuf[2 * k], voiceBuf[2 * k + 1]);
            else
                std::copy (voiceBuf, voiceBuf + n, hostBuf);

            for (int k = 0; k < n; ++k)
            {
                const int i = start + k;
                float l = 0.0f, r = 0.0f;
                transport.beat = i * beatStep;
                master->renderSample (hostBuf[k], hostBuf[k], l, r, macros, transport);
                outL[i] = l;
                outR[i] = r;
            }
        }
        else
        {
            for (int k = 0; k < vn; ++k)
            {
                transport.beat = (vstart + k) * voiceBeatStep;
                voice->renderSample (freq, 1.0, note, 1.0, macros, transport, 0.0,
                                     voiceBuf[k], voiceBufR[k]);
            }
            if (os == 2)
            {
                for (int k = 0; k < n; ++k)
                {
                    hostBuf[k] = decimator.process (voiceBuf[2 * k], voiceBuf[2 * k + 1]);
                    hostBufR[k] = decimatorR.process (voiceBufR[2 * k], voiceBufR[2 * k + 1]);
                }
            }
            else
            {
                std::copy (voiceBuf, voiceBuf + n, hostBuf);
                std::copy (voiceBufR, voiceBufR + n, hostBufR);
            }

            for (int k = 0; k < n; ++k)
            {
                const int i = start + k;
                float l = 0.0f, r = 0.0f;
                transport.beat = i * beatStep;
                master->renderSample (hostBuf[k], hostBufR[k], l, r, macros, transport);
                outL[i] = l;
                outR[i] = r;
            }
        }
    }
}

double computeBufferRms (const float* left, const float* right, int numSamples)
{
    if (left == nullptr || numSamples <= 0)
        return 0.0;
    double sum = 0.0;
    for (int i = 0; i < numSamples; ++i)
    {
        const double l = static_cast<double> (left[i]);
        const double r = right != nullptr ? static_cast<double> (right[i]) : l;
        sum += 0.5 * (l * l + r * r);
    }
    return std::sqrt (sum / static_cast<double> (numSamples));
}

} // namespace

double computeSpectralFlatness (const float* left, const float* right, int numSamples, double sampleRate)
{
    if (left == nullptr || numSamples < 1024 || sampleRate <= 0.0)
        return 0.0;

    constexpr size_t N = 1024;
    constexpr size_t H = 512;
    std::vector<double> avgP (N / 2 + 1, 0.0);

    const int startOffset = 0;
    const int endLimit = std::min (numSamples, static_cast<int> (sampleRate * 0.8));
    int frameCount = 0;

    std::vector<std::complex<double>> aL (N);
    std::vector<std::complex<double>> aR (N);
    const bool stereo = (right != nullptr && right != left);

    for (int start = startOffset; start + static_cast<int> (N) <= endLimit; start += H)
    {
        for (size_t i = 0; i < N; ++i)
        {
            const double w = 0.5 * (1.0 - std::cos (2.0 * kPi * static_cast<double> (i) / static_cast<double> (N)));
            aL[i] = static_cast<double> (left[start + i]) * w;
            if (stereo)
                aR[i] = static_cast<double> (right[start + i]) * w;
        }
        radix2Fft (aL);
        if (stereo)
        {
            radix2Fft (aR);
            for (size_t k = 0; k <= N / 2; ++k)
                avgP[k] += 0.5 * (std::norm (aL[k]) + std::norm (aR[k]));
        }
        else
        {
            for (size_t k = 0; k <= N / 2; ++k)
                avgP[k] += std::norm (aL[k]);
        }
        ++frameCount;
    }

    if (frameCount == 0)
        return 0.0;

    for (size_t k = 0; k <= N / 2; ++k)
        avgP[k] /= static_cast<double> (frameCount);

    const double binHz = sampleRate / static_cast<double> (N);
    const size_t kMin = static_cast<size_t> (std::max (1, static_cast<int> (std::ceil (200.0 / binHz))));
    const size_t kMax = static_cast<size_t> (std::min (static_cast<int> (N / 2), static_cast<int> (std::floor (8000.0 / binHz))));
    if (kMin >= kMax)
        return 0.0;

    const size_t K = kMax - kMin + 1;
    double arith = 0.0;
    double sumLog = 0.0;
    constexpr double eps = 1.0e-30;

    for (size_t k = kMin; k <= kMax; ++k)
    {
        arith += avgP[k];
        sumLog += std::log (avgP[k] + eps);
    }
    arith /= static_cast<double> (K);
    if (arith < 1.0e-12)
        return 0.0;

    const double geom = std::exp (sumLog / static_cast<double> (K));
    return std::min (1.0, std::max (0.0, geom / arith));
}

std::vector<MacroScanResult> scanPatchMacros (const Patch& patch, double sampleRate, int oversampling)
{
    std::vector<MacroScanResult> results;
    const unsigned read = macrosRead (patch);
    if (read == 0u)
        return results;

    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int os = std::clamp (oversampling, 1, PatchProgram::kMaxOversampling);

    double defaultMacros[MacroSmoother::kNumMacros];
    for (int m = 0; m < MacroSmoother::kNumMacros; ++m)
        defaultMacros[m] = patch.macroDefault (m);

    std::vector<float> defL40, defR40, defL52, defR52;
    renderMacroNote (patch, 40, defaultMacros, sr, os, defL40, defR40);
    renderMacroNote (patch, 52, defaultMacros, sr, os, defL52, defR52);

    const double fDef40 = computeSpectralFlatness (defL40.data(), defR40.data(), (int) defL40.size(), sr);
    const double fDef52 = computeSpectralFlatness (defL52.data(), defR52.data(), (int) defL52.size(), sr);
    const double rmsDef40 = computeBufferRms (defL40.data(), defR40.data(), (int) defL40.size());
    const double rmsDef52 = computeBufferRms (defL52.data(), defR52.data(), (int) defL52.size());

    const double baseFlatness = 0.5 * (fDef40 + fDef52);
    const double baseRms = std::sqrt (0.5 * (rmsDef40 * rmsDef40 + rmsDef52 * rmsDef52));

    for (int m = 0; m < MacroSmoother::kNumMacros; ++m)
    {
        if (((read >> m) & 1u) == 0u)
            continue;

        MacroScanResult res;
        res.macro = m + 1;
        res.label = (m < static_cast<int> (patch.macros.size()) && ! patch.macros[(size_t) m].label.empty())
                        ? patch.macros[(size_t) m].label
                        : ("Macro " + std::to_string (m + 1));
        res.defaultFlatness = baseFlatness;

        double testMacros[MacroSmoother::kNumMacros];
        for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
            testMacros[i] = (i == m) ? 1.0 : defaultMacros[i];

        std::vector<float> at1L40, at1R40, at1L52, at1R52;
        renderMacroNote (patch, 40, testMacros, sr, os, at1L40, at1R40);
        renderMacroNote (patch, 52, testMacros, sr, os, at1L52, at1R52);

        const double fAt140 = computeSpectralFlatness (at1L40.data(), at1R40.data(), (int) at1L40.size(), sr);
        const double fAt152 = computeSpectralFlatness (at1L52.data(), at1R52.data(), (int) at1L52.size(), sr);
        const double rmsAt140 = computeBufferRms (at1L40.data(), at1R40.data(), (int) at1L40.size());
        const double rmsAt152 = computeBufferRms (at1L52.data(), at1R52.data(), (int) at1L52.size());

        res.at1Flatness = 0.5 * (fAt140 + fAt152);
        res.deltaFlatness = res.at1Flatness - res.defaultFlatness;

        const double testRms = std::sqrt (0.5 * (rmsAt140 * rmsAt140 + rmsAt152 * rmsAt152));
        res.levelChangeDb = (baseRms > 1.0e-12 && testRms > 1.0e-12)
                                ? 20.0 * std::log10 (testRms / baseRms)
                                : 0.0;

        // Calibrate: tonal at default (< 0.40), high flatness at 1 (>= 0.50), big jump (>= 0.20)
        res.isNoise = (res.defaultFlatness < 0.40 && res.at1Flatness >= 0.50 && res.deltaFlatness >= 0.20);

        results.push_back (res);
    }
    return results;
}

} // namespace vibesynth
