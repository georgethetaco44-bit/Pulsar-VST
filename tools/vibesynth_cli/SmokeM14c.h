#pragma once

// M14c smoketests (SPEC §13 item 14c):
//   - LLM judge in --batch: parsing, 1 retry on garbage, "no score" on failure
//   - Audio descriptors computation: brightness, noisiness, movement, attack, sub, stereo width
//   - Voice oscillator analysis and reese detection
//   - Keyword matching and synonym table
//   - Example selection (slot 1 best, slot 2 different category, neutral fallback)
//   - Recipe selection rules (tempo_sync and ott_glue gating)
//   - New examples and recipes lint & offline test
//   - User presets as local examples

namespace vibesynth_cli {

bool smokeM14c();

} // namespace vibesynth_cli
