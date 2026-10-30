#pragma once

// M4: vibesynth_cli benchmark (SPEC §4.2.1, §10.3). Report-only.

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
// each marked PASS/FAIL against its budget. Always returns 0.
int runBench();

} // namespace vibesynth_cli
