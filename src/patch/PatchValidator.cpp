// PatchProgram build + the SPEC §7.1 offline test matrix. Worker thread
// only (or the CLI). No fast-math: the NaN/Inf checks are bit-pattern
// tests (SPEC §11).

#include "patch/PatchValidator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

} // namespace

ValidationResult validatePatch (const Patch& patch, double sampleRate,
                                const std::atomic<bool>* cancel, double cpuScale,
                                int oversampling)
{
    ValidationResult result;
    const double scale = cpuScale >= 1.0 ? cpuScale : 1.0;
    const double deadlineMs = kDeadlineMs * scale;
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
                macros[m] = set == 0 ? 0.0 : set == 2 ? 1.0 : patch.macroDefault (m);

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
                // Includes the per-sample clock reads (~20 ns, ~0.1 % CPU).
                voiceNs += std::chrono::duration<double, std::nano> (t1 - t0).count();

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
                macros[m] = set == 0 ? 0.0 : set == 2 ? 1.0 : patch.macroDefault (m);

            // resetState() leaves the VMs as fresh as the validator's.
            voice->resetState();
            master->resetState();
            HalfbandDecimator decimator;
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
    std::vector<double> times;
    for (int r = -1; r < runs; ++r) // run -1: untimed warm-up
    {
        float acc = 0.0f;
        const auto t0 = Clock::now();
        for (int i = 0; i < samples; ++i)
        {
            transport.beat += step;
            acc += voice->renderSample (kFreq, 1.0, kNote, 0.9, macros, transport);
        }
        const auto t1 = Clock::now();
        sink = sink + acc;
        if (r >= 0)
            times.push_back (std::chrono::duration<double, std::nano> (t1 - t0).count() / samples);
    }
    std::sort (times.begin(), times.end());
    return times[times.size() / 2];
}

} // namespace vibesynth
