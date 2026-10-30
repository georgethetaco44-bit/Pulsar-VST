// M6 — Utilities.h part 5: 3-band upward + downward compressor (fx_ott).
// Method bodies live in LexiconRacks.cpp.
#pragma once

#include "engine/Utilities4.h"

namespace vibesynth {

// ---------------------------------------------------------------------------
// fx_ott (own, SPEC §5.2): the multiband "OTT" squash of modern bass music.
//
// Bands: a 3-way Linkwitz-Riley 4th-order split (lo = LP4 @ 120 Hz, then
// an allpass @ 2.5 kHz; mid/hi = LP4/HP4 @ 2.5 kHz of HP4 @ 120 Hz). LR4
// bands stay in phase with each other, so unequal band gains never make
// the bumps a complementary (in − lowpass) split does; they sum to an
// allpass of the input. Depth 0 returns the input itself, bit for bit; the
// wet signal fades in or out over 10 ms when depth leaves or reaches 0.
//
// Each band: peak follower (10 ms attack, 100 ms release) per sample; the
// gain curve (log/exp) runs at control rate (every 16 samples) and is
// ramped linearly per sample. Boost (upward gain and makeup) may lift a
// band only up to −13.5 dBFS (where makeup leaves a band sitting at the
// downward threshold): at control rate against a true peak hold (instant
// attack; a saw's high band is 1-2 sample needles), and per sample, so a
// note onset that arrives while the gain still carries the boost earned on
// the quiet tail before it drops the gain at once (never below unity).
// Quiet material keeps its full lift; loud onsets pass at unity with their
// punch, and a patch's peak doesn't jump +10 dB.
// Fixed curve, scaled in dB by depth:
//   above −18 dB: downward 4:1
//   below −36 dB: upward 1:3, at most +18 dB, fading out below −60 dB
//                 (so silence and noise floors stay down)
//   makeup +4.5 dB (keeps a −12 dB band at about −12 dB).

struct OttState
{
    static constexpr int kNumBands = 3;
    static constexpr int kInterval = 16;

    XoverBand loA, loB, loAp;      // LP4 @ 120 Hz, allpass @ 2.5 kHz
    XoverBand restA, restB;        // HP4 @ 120 Hz
    XoverBand midA, midB;          // LP4 @ 2.5 kHz of rest
    XoverBand hiA, hiB;            // HP4 @ 2.5 kHz of rest
    double env[kNumBands] = {};
    double fast[kNumBands] = {};
    double gain[kNumBands] = { 1.0, 1.0, 1.0 };
    double step[kNumBands] = {};
    double target[kNumBands] = { 1.0, 1.0, 1.0 };
    double atk = 1.0, rel = 1.0;
    double wet = 0.0, wetStep = 1.0;
    int countdown = 0;

    void init (double sampleRate) noexcept;
    void reset() noexcept;
    float process (float in, double depth) noexcept;
};

} // namespace vibesynth
