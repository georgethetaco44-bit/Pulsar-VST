// M4 — Utilities.h part 3: fractional comb + safe feedback (own DSP).
// Method bodies live in LexiconRacks.cpp.
#pragma once

namespace vibesynth {

// ---------------------------------------------------------------------------
// Fractional-delay comb (own; linear interp; buffer sized in the rack).

struct CombState
{
    float* buf = nullptr;      // owned by the rack, sized in init()
    int capacity = 0;          // samples
    int writePos = 0;

    void reset() noexcept
    {
        if (buf != nullptr && capacity > 0)
            for (int i = 0; i < capacity; ++i)
                buf[i] = 0.0f;
        writePos = 0;
    }

    float process (float in, double hz, double feedback, double sampleRate) noexcept;
};

// ---------------------------------------------------------------------------
// Safe feedback (own; 20 Hz DC block + slew limit + tanh, SPEC §5.2).

struct SafeFbState
{
    double dcPrevIn = 0.0;
    double dcPrevOut = 0.0;
    double last = 0.0;

    void reset() noexcept
    {
        dcPrevIn = 0.0;
        dcPrevOut = 0.0;
        last = 0.0;
    }

    float process (float x, double sampleRate) noexcept;
};

} // namespace vibesynth
