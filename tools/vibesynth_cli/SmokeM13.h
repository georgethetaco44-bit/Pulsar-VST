#pragma once

namespace vibesynth_cli {

// M13 smoketests (SPEC §9.8):
//   - LFO shapes and rates against reference values, host-beat sync,
//     retrigger vs free-running, S&H and smooth random range
//   - ADSR envelope stages
//   - per-voice macro modulation (independent across voices, master unmodulated)
//   - inspector knob modulation + tweak clamping in normalized and semitone space
//   - no-mod renders bit-identical across all 15 built-ins
//   - .pulsar and OverrideTable round-trip, --bake note, --render playback
//   - Undo, Discard, Change this node, Mutate and Generate rules with canned replies
//   - bounds safety (extreme depths, rates and envelope times never produce NaN)
bool smokeM13();

} // namespace vibesynth_cli
