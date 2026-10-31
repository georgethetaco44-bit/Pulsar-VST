// M4 — Racks.cpp part 1: VoiceRack init/reset/metering.

#include "engine/Lexicon.h"
#include "engine/MasterRack.h"

#include <cmath>

namespace vibesynth {

void VoiceRack::recordMeter (ModuleId module, int slot, float out) noexcept
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

void VoiceRack::recordStateless (ModuleId module, float out) noexcept
{
    const int m = static_cast<int> (module);
    if (m < 0 || m >= kNumModules)
        return;
    const int n = statelessCalls[m];
    if (n < kMaxSlotsPerModule - 1)
        statelessCalls[m] = static_cast<uint8_t> (n + 1);
    recordMeter (module, n, out);
}

// One-time setup (off the audio thread): sample rate, oscillator
// waveforms, buffers, precomputed coefficients and per-voice seeds. All
// module state is then cleared by reset().
void VoiceRack::init (double newSampleRate, uint32_t voiceSeed)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    const float sr = static_cast<float> (sampleRate);

    for (int i = 0; i < kNumOscSlots; ++i)
    {
        oscSine[i].Init (sr);   oscSine[i].SetAmp (1.0f);
        oscSaw[i].Init (sr);    oscSaw[i].SetAmp (1.0f);
        oscSaw[i].SetWaveform (daisysp::Oscillator::WAVE_POLYBLEP_SAW);
        oscTri[i].Init (sr);    oscTri[i].SetAmp (1.0f);
        oscTri[i].SetWaveform (daisysp::Oscillator::WAVE_POLYBLEP_TRI);
        oscSquare[i].Init (sr); oscSquare[i].SetAmp (1.0f);
        oscSquare[i].SetWaveform (daisysp::Oscillator::WAVE_POLYBLEP_SQUARE);
        oscSquare[i].SetPw (0.5f);
        oscSineCache[i] = OscCache(); oscSawCache[i] = OscCache();
        oscTriCache[i] = OscCache();  oscSquareCache[i] = OscCache();
    }

    // Per-voice seeds: the detuned supersaw phases (so the 7 saws never
    // start phase-aligned) and the Lorenz start points (so voices don't all
    // trace the same orbit). A local generator keeps the patch-visible
    // noise() sequence independent of this.
    NoiseState seedRng;
    seedRng.seed (voiceSeed ^ 0x5DEECE66u);
    for (int i = 0; i < kNumSupersawSlots; ++i)
    {
        supersaw[i] = SupersawState();
        for (int p = 0; p < SupersawState::kNumSides; ++p)
            supersaw[i].seedPhase[p] = 0.5 * (static_cast<double> (seedRng.next()) + 1.0);
    }
    for (int i = 0; i < kNumLorenzSlots; ++i)
    {
        lorenz[i].x0 = 1.0 + 0.5 * static_cast<double> (seedRng.next());
        lorenz[i].y0 = 1.0 + 0.5 * static_cast<double> (seedRng.next());
        lorenz[i].z0 = 1.0 + 0.5 * static_cast<double> (seedRng.next());
    }

    for (int i = 0; i < kNumEnvFollowSlots; ++i)
        envFollow[i].init (sampleRate);

    for (int i = 0; i < kNumCombSlots; ++i)
    {
        comb[i].buf = combBuf[i];
        comb[i].capacity = kCombCapacity;
    }

    for (int i = 0; i < kNumXoverSlots; ++i)
        xover[i].init (sampleRate);

    filterRate.init (sampleRate);

    for (int i = 0; i < kNumOttSlots; ++i)
        ott[i].init (sampleRate);

    noise.seed (voiceSeed);

    reset();
}

// Clears all module state without allocating (patch swap, hard-steal
// retrigger). Daisy modules are re-initialised and every setter cache is
// cleared so the next control tick re-applies all parameters; the own
// filters reset to "unset" (their first call snaps to its inputs).
void VoiceRack::reset() noexcept
{
    const float sr = static_cast<float> (sampleRate);

    for (int i = 0; i < kNumOscSlots; ++i)
    {
        oscSine[i].Reset (0.0f); oscSaw[i].Reset (0.0f);
        oscTri[i].Reset (0.0f);  oscSquare[i].Reset (0.0f);
    }
    for (int i = 0; i < kNumSupersawSlots; ++i)
        supersaw[i].reset();

    for (int i = 0; i < kNumSvfSlots; ++i)
    {
        fltLp[i].reset(); fltHp[i].reset(); fltBp[i].reset();
    }
    for (int i = 0; i < kNumLadderSlots; ++i)
        fltLadder[i].reset();

    // Init (not Retrigger): idle, level 0, gate low, so the next gate rise
    // starts a clean attack and a low gate stays silent.
    for (int i = 0; i < kNumAdsrSlots; ++i)
    {
        envAdsr[i].Init (sr, 1);
        adsrCache[i] = AdsrCache();
    }

    for (int i = 0; i < kNumEnvFollowSlots; ++i)
        envFollow[i].reset();

    for (int i = 0; i < kNumCrushSlots; ++i)
    {
        crush[i].Init();
        crushCache[i] = CrushCache();
    }

    for (int i = 0; i < kNumCombSlots; ++i)
        comb[i].reset();

    for (int i = 0; i < kNumOttSlots; ++i)
        ott[i].reset();

    for (int i = 0; i < kNumResonatorSlots; ++i)
    {
        resonator[i].Init (0.5f, 12, sr);
        resonCache[i] = ResonCache();
    }

    for (int i = 0; i < kNumStringSlots; ++i)
    {
        string_[i].Init (sr);
        string_[i].SetNonLinearity (0.0f);
        stringCache[i] = StringCache();
    }

    for (int i = 0; i < kNumLorenzSlots; ++i)
    {
        lorenz[i].reset();
        lorenzCache[i] = LorenzCache();
    }
    for (int i = 0; i < kNumDuffingSlots; ++i)
    {
        duffing[i].reset();
        duffingCache[i] = DuffingCache();
    }
    for (int i = 0; i < kNumSafeFbSlots; ++i)
        safeFb[i].reset();
    for (int i = 0; i < kNumXoverSlots; ++i)
        xover[i].reset();
    for (int m = 0; m < kNumModules; ++m)
    {
        for (int s = 0; s < kMaxSlotsPerModule; ++s)
            meters[m][s].store (0.0f, std::memory_order_relaxed);
        touched[m].store (0u, std::memory_order_relaxed);
    }
    beginSample();
}

} // namespace vibesynth
