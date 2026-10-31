#pragma once

// PatchProgram (SPEC §7.3) and the offline test matrix (SPEC §7.1).
//
// Both run off the audio thread. JUCE-free: part of vibesynth_engine, so
// the CLI and the plugin run exactly the same checks.

#include <atomic>
#include <memory>
#include <string>

#include "engine/NodeMeters.h"
#include "patch/Patch.h"
#include "patch/PatchEngine.h"

namespace vibesynth {

// Everything compiled from one patch for one sample rate: 10 EelVoices
// (VM + rack each), one EelMaster, the trim and the parsed patch. M7: the
// voices run at `oversampling` x the host rate (SPEC §4.2: 2 in the plugin;
// vibesynth_cli --os 1 for A/B), the master at the host rate. Built on
// a worker (or in prepareToPlay); its ownership moves to the audio thread
// through the swap queue and back through the retire queue. The audio
// thread never builds or frees one.
class PatchProgram
{
public:
    static constexpr int kNumVoices = 10;
    static constexpr int kDefaultOversampling = 2;  // SPEC §4.2
    static constexpr int kMaxOversampling = 2;

    // Runs the loop-nesting check, then compiles all 11 VMs. On failure
    // returns null and sets `error` ("loops nested too deep ..." or
    // "compile error in <section>: <EEL2 text>"). Trim comes from
    // patch.trim (set it from the validator first). `sampleRate` is the
    // host rate; `oversampling` is clamped to 1..kMaxOversampling.
    static std::unique_ptr<PatchProgram> build (const Patch& patch, double sampleRate,
                                                std::string& error,
                                                int oversampling = kDefaultOversampling);

    EelVoice& voice (int index) noexcept { return *voices_[index]; }
    EelMaster& master() noexcept { return *master_; }
    const Patch& patch() const noexcept { return patch_; }
    double sampleRate() const noexcept { return sampleRate_; }  // host rate (master)
    int oversampling() const noexcept { return oversampling_; }
    double voiceRate() const noexcept { return sampleRate_ * oversampling_; }
    float trim() const noexcept { return trim_; }

    // Per-voice rack seed (chaos initial state etc.), same for every build.
    static uint32_t voiceSeed (int index) noexcept
    {
        return 0x1000u + static_cast<uint32_t> (index) * 0x9E3779B1u;
    }

private:
    PatchProgram() = default;

    std::unique_ptr<EelVoice> voices_[kNumVoices];
    std::unique_ptr<EelMaster> master_;
    Patch patch_;
    double sampleRate_ = 48000.0;
    int oversampling_ = kDefaultOversampling;
    float trim_ = 1.0f;
};

struct ValidationResult
{
    PatchStatus status = PatchStatus::Ok;
    std::string error;         // user-readable reject reason; empty when ok
    bool cancelled = false;    // stopped early through `cancel`
    double trim = 1.0;         // gain putting the worst-case peak at -3 dBFS (<= 1)
    double peak = 0.0;         // worst-case |master output| before trim
    // Per-configuration peaks (note-major: notes 28, 40, 55 x macros all 0,
    // defaults, all 1); 0 for configurations that never ran.
    double configPeak[9] = {};
    double configDc[9] = {};   // |mean| over the last 0.5 s, same order
    double voiceCpu = 0.0;     // voice render time / audio time, all configs
    double configCpu = 0.0;    // worst configuration: voice + master time / its 1 s
    double wallMs = 0.0;       // whole matrix incl. compiling the test VMs
    // M9 (SPEC §5.1 rule 4, §9.6): every (module, slot) touched in any
    // configuration that ran, the node visualizer's node list.
    TouchedNodes touched;

    bool ok() const noexcept { return status == PatchStatus::Ok && ! cancelled; }
};

// Runs the SPEC §7.1 matrix at `sampleRate` on fresh test VMs (never on a
// PatchProgram's voices): notes {28, 40, 55} x macros {all 0, defaults,
// all 1}, note-on at 0, note-off at 0.7 s, 1 s each, measured on the
// master output before trim/limiter. M7: through the plugin's voice path,
// i.e. the voice at `oversampling` x `sampleRate`, the same half-band
// decimator, then the master at `sampleRate`. The lexicon lint (M8), the
// loop-nesting check and compile errors are reported first. `cancel` (optional) is polled with the
// deadline after every voice sample. A configuration whose voice + master
// rendering takes more than 50 % of real time (500 ms for its 1 s) is
// rejected as too slow: a runaway guard, not the §4.2.1 design budget.
// `cpuScale` multiplies the CPU limit and the deadline; the plugin always
// uses 1. Only vibesynth_cli raises it, when it runs as x86_64 under
// Rosetta, where JIT code is ~8x slower (SPEC §4.2.1) and no patch runs in
// real time anyway.
ValidationResult validatePatch (const Patch& patch, double sampleRate,
                                const std::atomic<bool>* cancel = nullptr,
                                double cpuScale = 1.0,
                                int oversampling = PatchProgram::kDefaultOversampling);

// M8 weight hint (SPEC §6.4): the voice section's cost in ns per
// (oversampled) voice-sample, measured as the selftest and --bench do: one
// fresh voice at 55 Hz, gate held, macros at the patch defaults, beat
// advancing at 120 bpm, the median of `runs` x `samples` after one untimed
// pass. (The matrix's own timing reads the clock every sample, so it runs
// high.) 0 if the voice doesn't compile. Worker thread or the CLI.
double measureVoiceNs (const Patch& patch, double sampleRate, int runs = 3, int samples = 24000,
                       int oversampling = PatchProgram::kDefaultOversampling);

// M9 (SPEC §9.6): the touched (module, slot) set for a patch that is only
// compiled (built-ins, restores): a short silent render of the validator's
// 9 configurations (0.5 s each, note-off at 0.35 s) on one test voice and
// master, reset between configurations. Nothing is measured or rejected.
// Empty if the patch doesn't compile or `cancel` fires. Worker thread or
// the CLI.
TouchedNodes probeTouchedNodes (const Patch& patch, double sampleRate,
                                const std::atomic<bool>* cancel = nullptr,
                                int oversampling = PatchProgram::kDefaultOversampling);

// "compile error in voice: <text>" (section omitted when empty).
std::string formatCompileError (const std::string& section, const std::string& text);

} // namespace vibesynth
