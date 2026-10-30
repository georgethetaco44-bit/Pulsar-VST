#pragma once

// M6: vibesynth_cli --render (SPEC §10.3). See Render.cpp.

namespace vibesynth_cli {

// args: <patch.pulsar | built-in name> <out.wav> [--sr HZ]
// [--seconds-tail S] [--bpm BPM] [--macro N=V]... `cpuScale` is the validator's CPU
// allowance (10 under Rosetta). Returns 0 on success, 1 if the patch is
// rejected or the WAV can't be written, 2 on bad arguments.
int runRender (int argc, char** argv, double cpuScale);

} // namespace vibesynth_cli
