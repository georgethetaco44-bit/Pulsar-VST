// M4 — Racks2.cpp: MasterRack + FdnReverbState init/reset/metering
// (M12 Tier 2: bus_delay and bus_width state too).

#include "engine/Lexicon.h"
#include "engine/MasterRack.h"

#include <cmath>
#include <cstring>

namespace vibesynth {

namespace {

// DaisySP's ChorusEngine::Init calls SetLfoFreq, which reads the not yet
// initialised lfo_freq_ to pick the LFO direction, so the chorus depended
// on heap contents (M5: the validator's trim changed between runs). Zero
// the object first; it holds only floats and a fixed delay line, nothing
// to allocate. Also used by reset(), so the runtime guard (SPEC 7.2)
// clears the delay lines too.
void initChorus (daisysp::ChorusEngine& c, float sr, float lfoHz) noexcept
{
    std::memset (static_cast<void*> (&c), 0, sizeof (c));
    c.Init (sr);
    c.SetLfoFreq (lfoHz);
    c.SetFeedback (0.2f);
    c.SetDelayMs (14.0f);
}

} // namespace

void MasterRack::recordMeter (ModuleId module, int slot, float out) noexcept
{
    const int m = static_cast<int> (module);
    if (m < 0 || m >= kNumModules || slot < 0 || slot >= kMaxSlotsPerModule)
        return;
    const float mag = std::fabs (sanitizeFloat (out));
    const float prev = meters[m][slot].load (std::memory_order_relaxed);
    if (mag > prev)
        meters[m][slot].store (mag, std::memory_order_relaxed);
    // Plain load first: the locked RMW only runs on the first touch.
    const uint32_t bit = static_cast<uint32_t> (1u << slot);
    if ((touched[m].load (std::memory_order_relaxed) & bit) == 0u)
        touched[m].fetch_or (bit, std::memory_order_relaxed);
}

void FdnReverbState::reset() noexcept
{
    for (int l = 0; l < kNumLines; ++l)
    {
        if (buf[l] != nullptr && capacity[l] > 0)
            for (int i = 0; i < capacity[l]; ++i)
                buf[l][i] = 0.0f;
        writePos[l] = 0;
        lpState[l] = 0.0f;
    }
}

void MasterRack::init (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;

    // The chorus engines are (re)initialised in reset(), called below.
    chorusMix = 0.5f;

    monoXoverL.init (sampleRate);
    monoXoverR.init (sampleRate);
    monoRightHi = 0.0f;

    // FDN storage: worst case = max base × (sr/48k) × 1.5 + margin.
    // combBuf-style: the owner allocates reverbStorage before calling init
    // (see PatchEngine for sizes); init only carves it up.
    const double scale = sampleRate / 48000.0 * 1.5;
    int offset = 0;
    for (int l = 0; l < FdnReverbState::kNumLines; ++l)
    {
        const int need = static_cast<int> (FdnReverbState::kBaseLen[l] * scale) + 64;
        reverb.buf[l] = (reverbStorage != nullptr && offset + need <= reverbCapacity)
                        ? reverbStorage + offset : nullptr;
        reverb.capacity[l] = (reverb.buf[l] != nullptr) ? need : 0;
        reverb.writePos[l] = 0;
        reverb.delayLen[l] = static_cast<float> (FdnReverbState::kBaseLen[l]);
        reverb.lpState[l] = 0.0f;
        offset += need;
    }

    delay.init (sampleRate, delayStorage, delayFrames);
    width.init (sampleRate);

    reset();
}

void MasterRack::reset() noexcept
{
    const float sr = static_cast<float> (sampleRate);
    initChorus (chorusL, sr, 0.7f);
    initChorus (chorusR, sr, 0.9f);
    monoXoverL.reset();
    monoXoverR.reset();
    monoRightHi = 0.0f;
    reverb.reset();
    reverbRight = 0.0f;
    chorusRight = 0.0f;
    delay.reset();  // O(1): stale buffer contents are never read
    delayRight = 0.0f;
    width.reset();
    widthRight = 0.0f;
    for (int m = 0; m < kNumModules; ++m)
    {
        for (int s = 0; s < kMaxSlotsPerModule; ++s)
            meters[m][s].store (0.0f, std::memory_order_relaxed);
        touched[m].store (0u, std::memory_order_relaxed);
    }
}

} // namespace vibesynth
