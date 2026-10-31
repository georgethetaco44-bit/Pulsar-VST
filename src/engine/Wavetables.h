#pragma once

// M12: procedural wavetable oscillator (osc_wt), 2-op FM oscillator (osc_fm),
// hard-sync oscillator (osc_sync), and tempo-synced LFO (mod_lfo).
//
// Wavetable memory is initialized once at startup on the message/init thread
// via initWavetables() and is read-only on the audio thread.

#include <cmath>
#include <cstdint>

#include "engine/Utilities.h"

namespace vibesynth {

constexpr int kNumWavetables = 8;
constexpr int kNumWtFrames = 16;
constexpr int kWtFrameSize = 512;
constexpr int kNumWtMips = 9; // mip m has max harmonic 2^(8 - m): 256 down to 1

constexpr int kNumWtOscSlots = 2;
constexpr int kNumFmOscSlots = 2;
constexpr int kNumSyncOscSlots = 2;
constexpr int kNumModLfoSlots = 3;

// Initializes the 8 procedural wavetables and their band-limited mipmaps.
// Safe to call multiple times; runs once off the audio thread.
void initWavetables() noexcept;
bool wavetablesInitialised() noexcept;

// Returns the band-limited mip level (0..kNumWtMips-1) for fundamental `hz`
// at `sampleRate` so the highest partial stays below Nyquist.
int selectWtMip (double hz, double sampleRate) noexcept;

// Direct read of a wavetable sample for testing/inspection.
float wavetableSample (int table, int frame, int mip, int index) noexcept;

// ---------------------------------------------------------------------------
// osc_wt(slot, hz, table, pos): 8 procedural tables, per-octave mipmaps,
// smooth pos morphing across 16 frames.
struct WtOscState
{
    double phase = 0.0;
    double smoothPos = 0.0;
    bool posInit = false;

    void reset() noexcept
    {
        phase = 0.0;
        smoothPos = 0.0;
        posInit = false;
    }

    double process (double hz, double table, double pos, double sampleRate) noexcept;
};

// ---------------------------------------------------------------------------
// osc_fm(slot, hz, ratio, index, feedback): 2-op phase modulation with
// modulator self-feedback. ratio 0..1 maps to 0.5..16 (snaps near integers).
struct FmOscState
{
    double cPhase = 0.0;
    double mPhase = 0.0;
    double prevMod1 = 0.0;
    double prevMod2 = 0.0;

    void reset() noexcept
    {
        cPhase = 0.0;
        mPhase = 0.0;
        prevMod1 = 0.0;
        prevMod2 = 0.0;
    }

    static double mapRatio (double ratioNorm) noexcept
    {
        const double r = 0.5 * std::exp2 (5.0 * clamp01 (ratioNorm));
        const double nearest = std::floor (r + 0.5);
        if (nearest >= 1.0 && std::fabs (r - nearest) <= 0.12)
            return nearest;
        return r;
    }

    double process (double hz, double ratio, double index, double feedback,
                    double sampleRate) noexcept;
};

// ---------------------------------------------------------------------------
// osc_sync(slot, hz, ratio, shape): hard-sync oscillator, slave at 1..16x
// master, saw->square by shape, PolyBLEP-corrected master and slave resets.
struct SyncOscState
{
    double masterPhase = 0.0;
    double slavePhase = 0.0;

    void reset() noexcept
    {
        masterPhase = 0.0;
        slavePhase = 0.0;
    }

    double process (double hz, double ratio, double shape, double sampleRate) noexcept;
};

// ---------------------------------------------------------------------------
// mod_lfo(slot, beats, shape): tempo-synced LFO reading host beat position.
// Period in beats, shape 0..1 across sine/tri/saw/square/S&H, unipolar 0..1,
// retriggers at note-on.
struct ModLfoState
{
    double anchorBeat = 0.0;
    int64_t lastCycle = -1;
    double shValue = 0.5;
    uint32_t rngState = 0x13579BDFu;

    void reset (uint32_t seed = 0x13579BDFu) noexcept
    {
        anchorBeat = 0.0;
        lastCycle = -1;
        shValue = 0.5;
        rngState = seed != 0u ? seed : 0x13579BDFu;
    }

    void noteOn (double beat) noexcept
    {
        anchorBeat = sanitizeFloat (beat);
        lastCycle = -1;
    }

    double process (double currentBeat, double beats, double shape) noexcept;
};

} // namespace vibesynth
