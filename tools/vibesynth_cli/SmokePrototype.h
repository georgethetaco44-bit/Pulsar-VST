#pragma once

// Prototype smoketests:
// - stereo patch L != R and no NaN at macro corners
// - spread 0 gives L == R
// - mono patch gets inl == inr
// - unloaded table falls back safely
// - bad WAV is skipped
// - unison voice count sweeping under load neither clicks nor allocates
// - mod_curve with bad data stays safe and in range, deterministic for same beat

namespace vibesynth_cli {

bool smokePrototype();

} // namespace vibesynth_cli
