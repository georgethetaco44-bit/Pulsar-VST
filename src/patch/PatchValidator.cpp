// PatchProgram build + the SPEC §7.1 offline test matrix. Worker thread
// only (or the CLI). No fast-math: the NaN/Inf checks are bit-pattern
// tests (SPEC §11).

#include "patch/PatchValidator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace vibesynth {

std::string formatCompileError (const std::string& section, const std::string& text)
{
    if (section.empty())
        return "compile error: " + text;
    return "compile error in " + section + ": " + text;
}

std::unique_ptr<PatchProgram> PatchProgram::build (const Patch& patch, double sampleRate,
                                                   std::string& error)
{
    std::unique_ptr<PatchProgram> prog (new PatchProgram());
    prog->patch_ = patch;
    prog->sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
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
        prog->voices_[i] = std::make_unique<EelVoice> (ps, prog->sampleRate_, voiceSeed (i));
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
constexpr int kCheckEvery = 64;           // deadline check stride (samples)
constexpr double kDeadlineMs = 100.0;     // 10 % of a 1 s config
constexpr double kMaxVoiceCpu = 0.10;
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
                                const std::atomic<bool>* cancel, double cpuScale)
{
    ValidationResult result;
    const double scale = cpuScale >= 1.0 ? cpuScale : 1.0;
    const double deadlineMs = kDeadlineMs * scale;
    const double maxVoiceCpu = kMaxVoiceCpu * scale;
    const auto wallStart = Clock::now();
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int total = static_cast<int> (std::lround (sr * kConfigSeconds));
    const int noteOff = static_cast<int> (std::lround (sr * kNoteOffSeconds));
    const int dcStart = total - static_cast<int> (std::lround (sr * kDcWindowSeconds));

    const PatchStrings ps = { patch.init.c_str(), patch.voice.c_str(), patch.master.c_str() };
    float voiceBuf[kCheckEvery];
    double voiceNs = 0.0;
    double worstPeak = 0.0;
    auto finish = [&] (ValidationResult r)
    {
        r.peak = worstPeak;
        r.wallMs = msSince (wallStart, Clock::now());
        return r;
    };

    for (const int note : kNotes)
    {
        const double freq = 440.0 * std::pow (2.0, (note - 69) / 12.0);
        for (int set = 0; set < kNumMacroSets; ++set)
        {
            double macros[MacroSmoother::kNumMacros];
            for (int m = 0; m < MacroSmoother::kNumMacros; ++m)
                macros[m] = set == 0 ? 0.0 : set == 2 ? 1.0 : patch.macroDefault (m);

            // Fresh test VMs for every configuration (SPEC §7.1).
            const auto voice = std::make_unique<EelVoice> (ps, sr, PatchProgram::voiceSeed (0));
            if (! voice->isValid())
                return finish (reject (result, PatchStatus::CompileError,
                                       formatCompileError (voice->compileSection(),
                                                           voice->compileError())));
            const auto master = std::make_unique<EelMaster> (ps, sr);
            if (! master->isValid())
                return finish (reject (result, PatchStatus::CompileError,
                                       formatCompileError ("master", master->compileError())));

            double peak = 0.0;
            double sumL = 0.0, sumR = 0.0;
            const auto configStart = Clock::now();
            voice->noteOn (freq, note, 1.0);

            for (int start = 0; start < total; start += kCheckEvery)
            {
                const int n = std::min (kCheckEvery, total - start);
                const auto t0 = Clock::now();
                for (int k = 0; k < n; ++k)
                {
                    const double gate = start + k < noteOff ? 1.0 : 0.0;
                    voiceBuf[k] = voice->renderSample (freq, gate, note, 1.0, macros);
                }
                const auto t1 = Clock::now();
                voiceNs += std::chrono::duration<double, std::nano> (t1 - t0).count();

                for (int k = 0; k < n; ++k)
                {
                    const int i = start + k;
                    const float v = voiceBuf[k];
                    if (! isFiniteBits (v))
                        return finish (reject (result, PatchStatus::NonFinite,
                                               "NaN or Inf in voice output at " + where (note, set)
                                                   + ", after " + fmt ("%.2f s", i / sr)));
                    float l = 0.0f, r = 0.0f;
                    master->renderSample (v, v, l, r, macros);
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

                // Runaway guard (SPEC §7.1): never wait out a slow patch.
                if (msSince (configStart, Clock::now()) > deadlineMs)
                    return finish (reject (result, PatchStatus::TooSlow,
                                           "too slow: over " + fmt ("%.0f", deadlineMs)
                                               + " ms to render 1 s (" + where (note, set) + ")"));
                if (cancel != nullptr && cancel->load (std::memory_order_relaxed))
                {
                    result.cancelled = true;
                    return finish (reject (result, PatchStatus::TooSlow, "cancelled"));
                }
            }

            worstPeak = std::max (worstPeak, peak);
            if (peak > kMaxPeak)
                return finish (reject (result, PatchStatus::TooLoud,
                                       "too loud: peak " + fmt ("%+.1f", 20.0 * std::log10 (peak))
                                           + " dBFS (max +12) at " + where (note, set)));
            const double window = static_cast<double> (total - dcStart);
            const double dc = std::max (std::fabs (sumL), std::fabs (sumR)) / window;
            if (dc > kMaxDc)
                return finish (reject (result, PatchStatus::DcOffset,
                                       "DC offset " + fmt ("%.2f", dc) + " (max 0.1) at "
                                           + where (note, set)));
        }
    }

    if (worstPeak < kSilence)
        return finish (reject (result, PatchStatus::Silent, "patch makes no sound"));

    const double audioNs = 1.0e9 * kConfigSeconds * static_cast<double> (std::size (kNotes) * kNumMacroSets);
    result.voiceCpu = voiceNs / audioNs;
    if (result.voiceCpu > maxVoiceCpu)
        return finish (reject (result, PatchStatus::TooSlow,
                               "too slow: voice uses " + fmt ("%.0f", 100.0 * result.voiceCpu)
                                   + "% of real time (max " + fmt ("%.0f", 100.0 * maxVoiceCpu) + "%)"));

    result.trim = std::min (1.0, quantisePatchNumber (kTrimTarget / worstPeak));
    if (result.trim <= 0.0)
        result.trim = 1.0e-6;
    return finish (result);
}

} // namespace vibesynth
