#pragma once

// M14e smoketests (SPEC §9.7, §13 item 14e):
//   - Takes request and directions (1..4 count, labels, take lines)
//   - Takes N execution and take lines in prompt
//   - 429 fallback and cancel for Takes
//   - All-fail vs one-pass strip opening
//   - Passing takes reaching History
//   - Keep, Discard and Undo behavior
//   - Batch takes options and oscillator detection

namespace vibesynth_cli {

bool smokeM14e();

} // namespace vibesynth_cli
