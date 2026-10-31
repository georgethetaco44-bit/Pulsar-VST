#pragma once

// M8: vibesynth_cli --batch (SPEC §10.3). See Batch.cpp.

namespace vibesynth_cli {

// args: <prompts.txt> [--batch-limit N] [--out DIR] [--endpoint URL]
// [--model ID] [--effort low|minimal|medium|none]. Runs every prompt
// (one per line; blank lines and # comments skipped) through the full
// generation pipeline with the real service, saves each patch, a render
// and the raw replies, and prints per-prompt lines and totals. Needs an
// API key (VIBESYNTH_API_KEY or the settings file). Returns 0 when the
// batch ran (whatever the pass rate), 2 on bad arguments or no key.
int runBatch (int argc, char** argv, double cpuScale, int oversampling);

} // namespace vibesynth_cli
