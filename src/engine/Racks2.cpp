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

namespace {

int reverbLineCapacity (int line, double sampleRate) noexcept
{
    const double scale = sampleRate / 48000.0;
    return static_cast<int> (std::ceil ((FdnReverbState::kBaseLen[line] + FdnReverbState::kModDepth48k) * scale))
           + FdnReverbState::kMargin;
}

int reverbDiffuserLength (int d, double sampleRate) noexcept
{
    const int n = static_cast<int> (std::lround (FdnReverbState::kDiffLen[d] * sampleRate / 48000.0));
    return n < 1 ? 1 : n;
}

} // namespace

int FdnReverbState::storageFor (double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    int need = 0;
    for (int l = 0; l < kNumLines; ++l)
        need += reverbLineCapacity (l, sr);
    for (int d = 0; d < kNumDiffusers; ++d)
        need += reverbDiffuserLength (d, sr);
    return need;
}

void FdnReverbState::init (double sampleRate, float* storage, int capacityFloats) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double scale = sr / 48000.0;
    int offset = 0;
    for (int l = 0; l < kNumLines; ++l)
    {
        const int need = reverbLineCapacity (l, sr);
        buf[l] = (storage != nullptr && offset + need <= capacityFloats) ? storage + offset : nullptr;
        capacity[l] = buf[l] != nullptr ? need : 0;
        baseLen[l] = static_cast<float> (kBaseLen[l] * scale);
        offset += need;
    }
    for (int d = 0; d < kNumDiffusers; ++d)
    {
        const int need = reverbDiffuserLength (d, sr);
        diffBuf[d] = (storage != nullptr && offset + need <= capacityFloats) ? storage + offset : nullptr;
        diffLen[d] = diffBuf[d] != nullptr ? need : 0;
        offset += need;
    }
    // Damping: a one-pole low-pass at 8 kHz in each loop (DC gain 1).
    constexpr double kTwoPi = 6.28318530717958647692;
    const double fc = std::fmin (8000.0, 0.4 * sr);
    damp = static_cast<float> (1.0 - std::exp (-kTwoPi * fc / sr));
    dcPole = static_cast<float> (1.0 - kTwoPi * 10.0 / sr);
    glide = static_cast<float> (1.0 - std::exp (-1.0 / (0.005 * sr)));
    modDepth = static_cast<float> (kModDepth48k * scale);
    // Line modulation: 0.37 Hz and 0.61 Hz.
    const double rates[2] = { 0.37, 0.61 };
    for (int k = 0; k < 2; ++k)
    {
        rotC[k] = std::cos (kTwoPi * rates[k] / sr);
        rotS[k] = std::sin (kTwoPi * rates[k] / sr);
    }
    cachedSize = -1.0;
    cachedMix = -1.0;
    reset();
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
        dcIn[l] = 0.0f;
        dcOut[l] = 0.0f;
    }
    for (int d = 0; d < kNumDiffusers; ++d)
    {
        if (diffBuf[d] != nullptr)
            for (int i = 0; i < diffLen[d]; ++i)
                diffBuf[d][i] = 0.0f;
        diffPos[d] = 0;
    }
    for (int k = 0; k < 2; ++k)
    {
        lfoC[k] = 1.0;
        lfoS[k] = 0.0;
    }
    primed = false;
    mixPrimed = false;
    sizeHold = 0;
}

void MasterRack::init (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;

    // The chorus engines are (re)initialised in reset(), called below.
    chorusMix = 0.5f;

    monoXoverL.init (sampleRate);
    monoXoverR.init (sampleRate);
    monoRightHi = 0.0f;

    // FDN storage: the owner allocates reverbStorage (FdnReverbState::
    // storageFor) before calling init; init only carves it up.
    reverb.init (sampleRate, reverbStorage, reverbCapacity);

    delay.init (sampleRate, delayStorage, delayFrames);
    width.init (sampleRate);
    mb.init (sampleRate);

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
    mb.reset();
    mbRight = 0.0f;
    for (int m = 0; m < kNumModules; ++m)
    {
        for (int s = 0; s < kMaxSlotsPerModule; ++s)
            meters[m][s].store (0.0f, std::memory_order_relaxed);
        touched[m].store (0u, std::memory_order_relaxed);
    }
}

} // namespace vibesynth
