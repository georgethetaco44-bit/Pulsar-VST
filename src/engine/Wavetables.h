#pragma once

// M12: procedural wavetable oscillator (osc_wt), 2-op FM oscillator (osc_fm),
// hard-sync oscillator (osc_sync), and tempo-synced LFO (mod_lfo).
//
// Wavetable memory is initialized once at startup on the message/init thread
// via initWavetables() and is read-only on the audio thread.

#include <cmath>
#include <cstdint>

#include "engine/LoadedWavetables.h"
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
constexpr int kNumUnisonOscSlots = 2;
constexpr int kMaxUnisonVoices = 16;
constexpr int kNumModCurveSlots = 3;

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
// osc_unison(slot, hz, table, pos, voices, detune, spread): 1-16 voices,
// musical detune curve, equal-power pan, per-voice mip, normalization ~1/sqrt(n),
// randomized note-on phase. Returns L, caches R.
//
// Per sample: only the sounding voices read the table, and hz is still
// followed exactly. Every kUnisonControlInterval samples: the voice gains,
// detune ratios, pan gains and mips. Each voice's L/R gain ramps linearly
// to its new value across the next interval, so nothing steps. Voices at
// gain 0 only advance their phase, once per interval. When nothing is moving
// the interval does no exp, cos or sin at all.
constexpr int kUnisonControlInterval = 16;

struct UnisonOscState
{
    double phases[kMaxUnisonVoices] = {};
    double voiceGains[kMaxUnisonVoices] = {};
    double smoothT[kMaxUnisonVoices] = {};
    double ratios[kMaxUnisonVoices] = {};
    double gainL[kMaxUnisonVoices] = {};
    double gainR[kMaxUnisonVoices] = {};
    double stepL[kMaxUnisonVoices] = {};
    double stepR[kMaxUnisonVoices] = {};
    double targetL[kMaxUnisonVoices] = {};  // where the current ramp ends
    double targetR[kMaxUnisonVoices] = {};
    // Mip m of frame 0 for this voice; frame f is rowBase + f * frameStride.
    const float* rowBase[kMaxUnisonVoices] = {};
    double rowSize[kMaxUnisonVoices] = {};
    int rowMask[kMaxUnisonVoices] = {};
    double smoothPos = 0.0;
    double smoothDetune = 0.0;
    double smoothSpread = 0.0;
    double detuneCents = 0.0;
    double coeffRate = 0.0;     // the rate `coeff` was derived at
    double coeff = 0.0;         // one-pole per control interval (~8 ms)
    double lastF0 = -1.0;       // the hz the mips were chosen for
    const LoadedWavetable* lastLoaded = nullptr;
    int lastBuiltin = -1;
    int frameCount = 2;
    int frameStride = 0;
    int activeVoices = 0;       // voices 0..activeVoices-1 may sound
    int countdown = 0;          // samples until the next control update
    bool ramping = false;
    bool initialised = false;
    float lastR = 0.0f;

    void reset() noexcept
    {
        *this = UnisonOscState();
        voiceGains[0] = 1.0;
    }

    void noteOn (uint32_t seed) noexcept
    {
        uint32_t s = seed != 0u ? seed : 0x12345678u;
        for (int v = 0; v < kMaxUnisonVoices; ++v)
        {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            phases[v] = static_cast<double> (s & 0xFFFFFFu) / 16777216.0;
        }
        countdown = 0;  // a new note's pitch gets its own mips at once
    }

    double process (double hz, double table, double pos, double voices,
                    double detune, double spread, double sampleRate) noexcept;

private:
    void updateControl (double f0, double table, double voices, double detune, double spread,
                        double sampleRate) noexcept;
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

// ---------------------------------------------------------------------------
// mod_curve(slot, beats, base, points): tempo-synced drawn curve read from mem[].
// points 2-32; point i is mem[base + 3*i] = x, y, bend. The bend of point i
// shapes the segment from i to i + 1: t' = (exp(b t) - 1) / (exp(b) - 1), b
// clamped to -8..8, t' = t when |b| < 0.01. 1 ms jump smoothing on vertical
// jumps and wraps.
//
// Each sample reads and checks only the points the position needs: the two
// around it, plus the ones walked past after a wrap. mem[] may be rewritten
// at any time, so nothing but the running maximum of the earlier x values is
// cached; that maximum refreshes on the next wrap. Non-finite data in a point
// that is read outputs the decaying jump offset (0 when settled) until it is
// gone. Output stays in 0..1 and finite for any mem[] contents and arguments.
struct ModCurveState
{
    double prevPhase = 0.0;
    double prevRawY = 0.0;
    double jumpOffset = 0.0;
    double decayRate = 0.0;    // the rate `decay` was derived at
    double decay = 0.0;        // per sample, 1 ms time constant
    double prefixX = 0.0;      // largest x before point `segment`
    double cachedBend = 0.0;   // the bend `bendScale` was derived for
    double bendScale = 0.0;    // 1 / (exp(cachedBend) - 1)
    int segment = 0;           // the segment found last sample
    bool initialised = false;

    void reset() noexcept { *this = ModCurveState(); }

    // `restarted`: the position was moved on purpose, so smooth the jump as
    // for a wrap.
    double process (void* vm, double beat, double beatsIn, double baseIn, double pointsIn,
                    double sampleRate, bool restarted = false) noexcept;
};

// ---------------------------------------------------------------------------
// mod_curve_note(slot, beats, base, points): mod_curve whose phase is 0 on the
// note-on sample (`trig`) and counts beats from there. Legato notes have no
// trig, so they don't restart it. Beats are counted as sample-to-sample
// differences, so a host loop or locate can't move the curve mid-note. There
// are no notes in master: it follows the transport there, exactly as mod_curve.
struct ModCurveNoteState
{
    // A larger move between two calls, or a backward one, is a transport jump.
    static constexpr double kMaxBeatStep = 0.25;

    ModCurveState curve;
    double elapsed = 0.0;
    double lastBeat = 0.0;
    double lastStep = 0.0;
    bool restartPending = false;

    void reset() noexcept { *this = ModCurveNoteState(); }
    void noteOn() noexcept { restartPending = true; }

    double process (void* vm, double beat, double beatsIn, double baseIn, double pointsIn,
                    double sampleRate) noexcept;
};

} // namespace vibesynth
