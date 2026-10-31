#pragma once

// M11: vibesynth_cli --change, --mutate and --change-batch (SPEC §9.7,
// §10.3). See Edit.cpp. Each needs an API key (VIBESYNTH_API_KEY or the
// settings file) and accepts [--endpoint URL] [--model ID]
// [--effort low|minimal|medium|none] like --batch. Return 0 when the run
// happened (whatever passed), 1 when --change's one edit failed, 2 on bad
// arguments or no key.

namespace vibesynth_cli {

// args: <in.pulsar | built-in> --node <id> --say "<instruction>" --out out.pulsar
int runChange (int argc, char** argv, int oversampling);

// args: <in.pulsar | built-in> --outdir DIR
int runMutate (int argc, char** argv, int oversampling);

// args: <cases.txt> [--out DIR]: one "preset | node | instruction" per
// line (blank lines and # comments skipped).
int runChangeBatch (int argc, char** argv, int oversampling);

} // namespace vibesynth_cli
