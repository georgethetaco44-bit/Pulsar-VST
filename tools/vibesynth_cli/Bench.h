#pragma once

// M4: vibesynth_cli benchmark (SPEC §4.2.1, §10.3). Report-only.

#include "patch/Patch.h"

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

} // namespace vibesynth_cli
