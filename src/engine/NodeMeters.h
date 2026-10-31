#pragma once

// M9: the node visualizer's data (SPEC §5.1 rule 4, §9.6, §11).
//
//   TouchedNodes   the (module, slot) pairs a patch really uses, split into
//                  the voice rack and the master racks. The validator fills
//                  it from the racks' touched bits during the offline test;
//                  compile-only loads get it from probeTouchedNodes()
//                  (PatchValidator.h). Plain data, built off the audio
//                  thread and handed to the UI by value.
//   NodeMeterBank  live peaks per (module, slot) that the audio thread
//                  publishes once per block (PatchPlayer::harvestMeters):
//                  the max over sounding voices since the UI last read it.
//                  Lock-free: the audio thread raises a value with a relaxed
//                  CAS, the UI takes it with a relaxed exchange. It never
//                  points into a program, so the UI can read it while
//                  programs are swapped and freed.
//
// JUCE-free, part of vibesynth_engine.

#include <atomic>
#include <cstdint>
#include <string>

#include "engine/Lexicon.h"

namespace vibesynth {

// "flt_lp", "bus_reverb", ...: the lexicon name of the function that meters
// as `module` (util_xover_lo/hi share "util_xover"). Empty for Count.
const char* moduleShortName (ModuleId module) noexcept;

// The module a graph-edge name refers to: "flt_lp", and also the aliases
// "util_xover_lo"/"util_xover_hi" and the "_r" companions of bus_*.
// Case-insensitive. False for names that don't meter (fx_fold, noise, ...).
bool moduleFromName (const std::string& name, ModuleId& module) noexcept;

struct TouchedNodes
{
    uint32_t voice[kNumModules] = {};   // bit s = slot s of that module
    uint32_t master[kNumModules] = {};  // the master's VoiceRack and MasterRack

    bool any() const noexcept
    {
        for (int m = 0; m < kNumModules; ++m)
            if ((voice[m] | master[m]) != 0u)
                return true;
        return false;
    }

    int count() const noexcept
    {
        int n = 0;
        for (int m = 0; m < kNumModules; ++m)
            for (uint32_t b = voice[m] | (master[m] << 16); b != 0u; b &= b - 1u)
                ++n;
        return n;
    }

    bool operator== (const TouchedNodes& o) const noexcept
    {
        for (int m = 0; m < kNumModules; ++m)
            if (voice[m] != o.voice[m] || master[m] != o.master[m])
                return false;
        return true;
    }
    bool operator!= (const TouchedNodes& o) const noexcept { return ! (*this == o); }

    // ORs a rack's touched bits into `dst` (off the audio thread).
    static void add (uint32_t dst[kNumModules], const std::atomic<uint32_t> src[kNumModules]) noexcept
    {
        for (int m = 0; m < kNumModules; ++m)
            dst[m] |= src[m].load (std::memory_order_relaxed);
    }
};

struct NodeMeterBank
{
    std::atomic<float> voice[kNumModules][kMaxSlotsPerModule];
    std::atomic<float> master[kNumModules][kMaxSlotsPerModule];

    NodeMeterBank() noexcept { clear(); }

    void clear() noexcept
    {
        for (int m = 0; m < kNumModules; ++m)
            for (int s = 0; s < kMaxSlotsPerModule; ++s)
            {
                voice[m][s].store (0.0f, std::memory_order_relaxed);
                master[m][s].store (0.0f, std::memory_order_relaxed);
            }
    }

    // Audio thread: raises `cell` to at least `v` (no allocation, no lock).
    static void raise (std::atomic<float>& cell, float v) noexcept
    {
        float cur = cell.load (std::memory_order_relaxed);
        while (v > cur && ! cell.compare_exchange_weak (cur, v, std::memory_order_relaxed))
        {
        }
    }

    // UI: the peak since the last take, and resets it.
    static float take (std::atomic<float>& cell) noexcept
    {
        return cell.exchange (0.0f, std::memory_order_relaxed);
    }
};

// Audio thread: moves a rack's meter peaks into `dst` (null: just clears
// them, for a voice that isn't sounding) and zeroes them in the rack, so
// each block publishes only what was played since the last one. Only
// slots with a touched bit are visited. The rack's own thread is the only
// writer of its meters, so the load + store pair can't lose a peak.
inline void harvestRackMeters (std::atomic<float> (&meters)[kNumModules][kMaxSlotsPerModule],
                               const std::atomic<uint32_t> (&touched)[kNumModules],
                               std::atomic<float> (*dst)[kMaxSlotsPerModule]) noexcept
{
    for (int m = 0; m < kNumModules; ++m)
    {
        uint32_t bits = touched[m].load (std::memory_order_relaxed);
        for (int s = 0; bits != 0u && s < kMaxSlotsPerModule; ++s, bits >>= 1)
        {
            if ((bits & 1u) == 0u)
                continue;
            const float v = meters[m][s].load (std::memory_order_relaxed);
            if (v <= 0.0f)
                continue;
            meters[m][s].store (0.0f, std::memory_order_relaxed);
            if (dst != nullptr)
                NodeMeterBank::raise (dst[m][s], v);
        }
    }
}

} // namespace vibesynth
