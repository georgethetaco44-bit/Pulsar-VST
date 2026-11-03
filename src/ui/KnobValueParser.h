#pragma once

// M14b value parser for typing values into knobs (SPEC §9.6, §9.7).
// Accepts what the readout shows, with or without unit, case-insensitive:
// "250", "250 Hz", "1.2k", "1.2 kHz", "-6 dB", "40%", "120 ms", "0.5 s",
// "1/8" for synced rates, "7 st" for semitones, plain 0–1 for macros as well
// as "50%". Inverts each knob's display mapping, clamps to range, rejects garbage.

#include <juce_core/juce_core.h>

namespace vibesynth::ui {

enum class KnobUnitKind
{
    Generic,
    Macro,
    Hz,
    Semitones,
    Decibels,
    Time,
    Glide,
    SyncedRate,
    Choice,
    FilterCutoff
};

struct KnobParseResult
{
    bool ok = false;
    double value = 0.0;
    bool isHz = false;
};

KnobParseResult parseKnobValue (const juce::String& text, KnobUnitKind kind,
                                double minVal = 0.0, double maxVal = 1.0);

} // namespace vibesynth::ui
