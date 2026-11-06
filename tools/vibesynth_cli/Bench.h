#pragma once

// M4: vibesynth_cli benchmark (SPEC §4.2.1, §10.3). Report-only.

#include <vector>

#include "patch/Patch.h"
#include "patch/PatchEngine.h"

namespace vibesynth_cli {

// One lexicon benchmark: a voice program and a master program. Master
// cases (bus_*) are timed on the master VM against the master budget;
// all others are timed on the voice VM against the voice budget.
struct BenchCase
{
    const char* name;
    const char* voice;
    const char* master;
    bool isMaster;
    const char* init = nullptr;
};

extern const BenchCase kBenchCases[];
extern const int kNumBenchCases;

// Prints ns per voice-sample for every case and the full test preset,
// each marked PASS/FAIL against its budget. M7: voice VMs run at
// `oversampling` x 48 kHz (the plugin's 2x voice path; --os 1 for A/B),
// masters at 48 kHz. Always returns 0.
int runBench (int oversampling);

// One patch's cost (55 Hz, gate held, macro defaults): the median of
// `runs` x `samples` for the voice (per voice-sample, at `oversampling` x
// 48 kHz) and for the master (per host sample, fed through the half-band
// decimator), each after one untimed pass (SPEC §4.2.1). ok = false if it
// doesn't compile.
struct PresetCost
{
    bool ok = false;
    double voiceNs = 0.0;
    double masterNs = 0.0;
};
PresetCost measurePresetCost (const vibesynth::Patch& patch, int runs, int samples,
                              int oversampling);

// Spins the CPU up to its sustained clock for 0.5 s (call once before a
// series of timed measurements).
void warmUp();

// The bench's own timing loops, for --cost-probe (CostProbe.cpp): the median
// of `runs` x `samples` after one untimed pass, gate held, beat at 120 bpm.
double benchTimeVoice (vibesynth::EelVoice& v, double freq, double note,
                       const double macros[vibesynth::MacroSmoother::kNumMacros],
                       double voiceRate, int runs, int samples);
double benchTimeMaster (vibesynth::EelMaster& m, const std::vector<float>& in,
                        const double macros[vibesynth::MacroSmoother::kNumMacros],
                        int runs, int samples);
std::vector<float> benchRenderInput (vibesynth::EelVoice& v, double freq,
                                     const double macros[vibesynth::MacroSmoother::kNumMacros],
                                     int os, int samples);

// Developer tool (phase 2 cost-jump study): measures one patch in this
// process and prints one line of key=value pairs. See CostProbe.cpp.
int runCostProbe (int argc, char** argv, int oversampling);

} // namespace vibesynth_cli
