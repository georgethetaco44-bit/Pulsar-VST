#pragma once

// Loadable high-resolution wavetables (Task 3).
//
// Tables 0-7 are built-in procedural wavetables; tables 8-39 are loaded
// from <app-data>/Wavetables/ at startup off the audio thread.
//
// Real-time rules (SPEC §11): zero allocation, locks, logging, I/O on the
// audio thread. Tables are published via std::atomic<const LoadedWavetable*>.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/Utilities.h"

namespace vibesynth {

constexpr int kMaxLoadedWavetables = 32;
constexpr int kBaseWtFrameSize = 2048;
constexpr int kMaxWtFrames = 256;
constexpr int kNumLoadedWtMips = 11; // 2048, 1024, 512, 256, 128, 64, 32, 16, 8, 4, 2
constexpr int kLoadedWtFrameStride = 4105; // (2048+1) + (1024+1) + ... + (2+1) floats

struct LoadedWavetable
{
    std::string name;
    int numFrames = 0;
    std::vector<float> data; // numFrames * kLoadedWtFrameStride floats
};

// Returns offset of mip m (0..10) within one frame of 4105 floats.
inline int loadedWtMipOffset (int mip) noexcept
{
    static constexpr int kOffsets[kNumLoadedWtMips] = {
        0, 2049, 3074, 3587, 3844, 3973, 4038, 4071, 4088, 4097, 4102
    };
    return kOffsets[mip >= 0 && mip < kNumLoadedWtMips ? mip : (kNumLoadedWtMips - 1)];
}

// Select mip for fundamental `hz` at `sampleRate` so partials stay below 0.45 * sampleRate.
inline int selectLoadedWtMip (double hz, double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double f = clampPitch (hz, sr);
    const double maxAllowedH = (0.45 * sr) / f;
    // Mip m has max harmonic (2048 >> m) / 2 = 1024 >> m.
    for (int mip = 0; mip < kNumLoadedWtMips - 1; ++mip)
    {
        const int h = 1024 >> mip;
        if (static_cast<double> (h) <= maxAllowedH)
            return mip;
    }
    return kNumLoadedWtMips - 1;
}

// Sample from a loaded wavetable.
// Audio thread safe: zero allocation, lock-free, O(1).
inline double sampleLoadedWavetable (const LoadedWavetable* wt, double f, double phase,
                                     double smoothPos, double sampleRate) noexcept
{
    if (wt == nullptr || wt->numFrames <= 0 || wt->data.empty())
        return 0.0;

    const int mip = selectLoadedWtMip (f, sampleRate);
    const int mipSize = 2048 >> mip;
    const int mipOff = loadedWtMipOffset (mip);
    const int numFrames = wt->numFrames;

    // Morph across frames
    const double fScaled = smoothPos * static_cast<double> (numFrames - 1);
    int f0 = static_cast<int> (fScaled);
    if (f0 >= numFrames - 1)
        f0 = numFrames - 2;
    if (f0 < 0)
        f0 = 0;
    const double fFrac = (numFrames > 1) ? (fScaled - static_cast<double> (f0)) : 0.0;

    // Read within frame at phase
    const double sScaled = phase * static_cast<double> (mipSize);
    const int s0 = static_cast<int> (sScaled) & (mipSize - 1);
    const double sFrac = sScaled - std::floor (sScaled);

    const float* row0 = wt->data.data() + f0 * kLoadedWtFrameStride + mipOff;
    const float* row1 = (numFrames > 1)
                            ? (wt->data.data() + (f0 + 1) * kLoadedWtFrameStride + mipOff)
                            : row0;

    const double v0 = row0[s0] + sFrac * (row0[s0 + 1] - row0[s0]);
    const double v1 = row1[s0] + sFrac * (row1[s0 + 1] - row1[s0]);
    return v0 + fFrac * (v1 - v0);
}

// Audio thread entry: lookup loaded table pointer for index >= 8 (returns nullptr if not loaded).
const LoadedWavetable* getLoadedWavetable (int index) noexcept;

// Non-audio thread: publish a loaded wavetable at slot (0..31, corresponding to table indices 8..39).
void setLoadedWavetable (int slot, std::unique_ptr<LoadedWavetable> wt) noexcept;

// Total count of loaded wavetables.
int getNumLoadedWavetables() noexcept;

} // namespace vibesynth
