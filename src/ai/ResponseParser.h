#pragma once

// Parses a model reply in the SPEC §6.2 layout into a Patch:
//
//   ```json  {"name", "voiceMode", "macros": [8 x {"label", "default"}], "graph"}
//   ```eel init     (optional)
//   ```eel voice    (required)
//   ```eel master   (optional; empty = passthrough)
//
// Tolerated: prose around the blocks, a missing init or master, ```eel2 /
// ```EEL fences (any case), and a bare ```voice. Refused (parse errors,
// which go through the self-heal retry): no JSON block, invalid JSON, no
// voice block, code inside JSON strings, a section over 32 KB. Macro
// defaults are clamped to 0..1 and the list is padded or truncated to 8;
// each fix-up is reported in `notes`.
//
// Compiled into the plugin and vibesynth_cli (juce::JSON). Worker thread.

#include <string>
#include <vector>

#include "patch/Patch.h"

namespace vibesynth {

struct ResponseParseResult
{
    PatchStatus status = PatchStatus::Ok;  // BadJson, BadField, MissingVoice or TooLarge on failure
    std::string error;                     // the self-heal reason
    Patch patch;                           // prompt and trim left at their defaults
    std::vector<std::string> notes;        // fix-ups ("padded macros 5 -> 8")

    bool ok() const noexcept { return status == PatchStatus::Ok; }
};

ResponseParseResult parseResponse (const std::string& reply);

} // namespace vibesynth
