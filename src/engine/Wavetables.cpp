// M12: procedural wavetables + osc_wt, osc_fm, osc_sync, mod_lfo.

#include "engine/Wavetables.h"

#include "eel2/ns-eel.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace vibesynth {

namespace {

constexpr int kMaxHarmonic = kWtFrameSize / 2; // 256
constexpr int kMask = kWtFrameSize - 1;        // 511
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

// [table][frame][mip][sample + 1 guard]
float gTables[kNumWavetables][kNumWtFrames][kNumWtMips][kWtFrameSize + 1];
std::atomic<bool> gInitialised { false };
std::once_flag gInitFlag;

inline double polyBlep (double t, double dt) noexcept
{
    if (dt <= 0.0)
        return 0.0;
    if (t < dt)
    {
        const double u = t / dt;
        return u + u - u * u - 1.0;
    }
    if (t > 1.0 - dt)
    {
        const double u = (t - 1.0) / dt;
        return u * u + u + u + 1.0;
    }
    return 0.0;
}

// Evaluates one frame's harmonic spectrum (cosAmp[1..256], sinAmp[1..256]).
void buildFrameSpectrum (int table, int frame, const double* cosLut, const double* sinLut,
                         double* cosAmp, double* sinAmp) noexcept
{
    const double pos = static_cast<double> (frame) / static_cast<double> (kNumWtFrames - 1);
    std::memset (cosAmp, 0, (kMaxHarmonic + 1) * sizeof (double));
    std::memset (sinAmp, 0, (kMaxHarmonic + 1) * sizeof (double));

    if (table == 0) // Basic: sine -> tri -> saw -> square
    {
        for (int k = 1; k <= kMaxHarmonic; ++k)
        {
            const double sineH = (k == 1) ? 1.0 : 0.0;
            const double triH = (k & 1) ? ((((k - 1) >> 1) & 1) ? -1.0 : 1.0) / (double) (k * k) : 0.0;
            const double sawH = ((k & 1) ? 1.0 : -1.0) / (double) k;
            const double sqH = (k & 1) ? 1.0 / (double) k : 0.0;
            if (pos < 1.0 / 3.0)
            {
                const double u = pos * 3.0;
                sinAmp[k] = (1.0 - u) * sineH + u * triH;
            }
            else if (pos < 2.0 / 3.0)
            {
                const double u = (pos - 1.0 / 3.0) * 3.0;
                sinAmp[k] = (1.0 - u) * triH + u * sawH;
            }
            else
            {
                const double u = (pos - 2.0 / 3.0) * 3.0;
                sinAmp[k] = (1.0 - u) * sawH + u * sqH;
            }
        }
        return;
    }
    if (table == 1) // PWM: duty cycle 0.50 -> 0.06
    {
        const double pw = 0.50 - 0.44 * pos;
        for (int k = 1; k <= kMaxHarmonic; ++k)
            sinAmp[k] = std::sin (k * kPi * pw) / (double) k;
        return;
    }
    if (table == 2) // Vocal / formant (a -> e -> i -> o -> u)
    {
        static constexpr double kFormants[5][3] = {
            { 6.6, 9.9, 22.2 },  // a
            { 4.8, 16.7, 22.5 }, // e
            { 2.5, 20.8, 27.4 }, // i
            { 5.2, 7.6, 21.9 },  // o
            { 2.7, 7.9, 20.4 },  // u
        };
        static constexpr double kGains[3] = { 1.0, 0.65, 0.35 };
        const double vPos = pos * 4.0;
        const int v0 = std::min (3, (int) vPos);
        const double vf = vPos - v0;
        double fCenter[3];
        for (int b = 0; b < 3; ++b)
            fCenter[b] = (1.0 - vf) * kFormants[v0][b] + vf * kFormants[v0 + 1][b];
        for (int k = 1; k <= kMaxHarmonic; ++k)
        {
            double env = 0.12 / (double) k;
            for (int b = 0; b < 3; ++b)
            {
                const double d = ( (double) k - fCenter[b]) / (1.2 + 0.18 * fCenter[b]);
                env += kGains[b] * std::exp (-0.5 * d * d);
            }
            sinAmp[k] = env * ((k & 1) ? 1.0 : -0.8);
        }
        return;
    }
    if (table == 3) // Bell: sparse high partials
    {
        static constexpr int kPartials[] = { 1, 2, 3, 5, 7, 9, 11, 14, 17, 21, 26, 31, 38, 47 };
        for (size_t i = 0; i < sizeof (kPartials) / sizeof (kPartials[0]); ++i)
        {
            const int k = kPartials[i];
            const double rank = (double) i / 13.0;
            const double d = rank - (0.15 + 0.70 * pos);
            const double w = std::exp (-3.5 * d * d) / std::pow ((double) k, 0.25);
            sinAmp[k] = ((i & 1) ? -w : w);
        }
        sinAmp[1] += 0.35 * (1.0 - 0.5 * pos);
        return;
    }
    if (table == 5) // Harmonic comb
    {
        const double notchRate = 0.06 + 0.38 * pos;
        for (int k = 1; k <= kMaxHarmonic; ++k)
        {
            const double c = std::cos (k * kPi * notchRate);
            sinAmp[k] = (c * c) / std::pow ((double) k, 0.72);
        }
        sinAmp[1] = std::fmax (sinAmp[1], 0.4);
        return;
    }

    // Time-domain tables (4 = sync sweep, 6 = fold sweep, 7 = growl), analyzed via LUT DFT.
    double wave[kWtFrameSize];
    double mean = 0.0;
    for (int n = 0; n < kWtFrameSize; ++n)
    {
        const double t = (double) n / (double) kWtFrameSize;
        double y = 0.0;
        if (table == 4) // Sync sweep
        {
            const double syncRatio = 1.0 + 7.5 * pos;
            const double sp = syncRatio * t - std::floor (syncRatio * t);
            const double win = 0.5 - 0.5 * cosLut[n];
            y = ((2.0 * sp - 1.0) * 0.65 + std::sin (kTwoPi * syncRatio * t) * 0.35) * win;
        }
        else if (table == 6) // Fold sweep
        {
            const double gain = 1.0 + 6.5 * pos;
            y = std::sin (gain * kHalfPi * sinLut[n]);
        }
        else // Table 7: Growl
        {
            const double idx1 = 1.0 + 3.4 * pos;
            const double idx2 = 0.3 + 2.0 * pos;
            const double mod = idx1 * sinLut[(2 * n) & kMask] + idx2 * sinLut[(3 * n) & kMask];
            const double env = 0.65 - 0.35 * cosLut[n];
            y = std::sin (kTwoPi * t + mod) * env;
        }
        wave[n] = y;
        mean += y;
    }
    mean /= (double) kWtFrameSize;
    const double scale = 2.0 / (double) kWtFrameSize;
    for (int k = 1; k <= kMaxHarmonic; ++k)
    {
        double cSum = 0.0, sSum = 0.0;
        for (int n = 0; n < kWtFrameSize; ++n)
        {
            const int idx = (k * n) & kMask;
            const double v = wave[n] - mean;
            cSum += v * cosLut[idx];
            sSum += v * sinLut[idx];
        }
        cosAmp[k] = cSum * scale;
        sinAmp[k] = sSum * scale;
    }
}

void buildAllWavetables() noexcept
{
    double cosLut[kWtFrameSize];
    double sinLut[kWtFrameSize];
    for (int n = 0; n < kWtFrameSize; ++n)
    {
        const double a = kTwoPi * (double) n / (double) kWtFrameSize;
        cosLut[n] = std::cos (a);
        sinLut[n] = std::sin (a);
    }

    double cosAmp[kMaxHarmonic + 1];
    double sinAmp[kMaxHarmonic + 1];
    double acc[kWtFrameSize];

    for (int t = 0; t < kNumWavetables; ++t)
    {
        for (int f = 0; f < kNumWtFrames; ++f)
        {
            buildFrameSpectrum (t, f, cosLut, sinLut, cosAmp, sinAmp);

            // Normalize full-bandwidth energy so all frames/tables have matched level.
            double energy = 0.0;
            for (int k = 1; k <= kMaxHarmonic; ++k)
                energy += 0.5 * (cosAmp[k] * cosAmp[k] + sinAmp[k] * sinAmp[k]);
            const double rmsGain = energy > 1.0e-12 ? (0.55 / std::sqrt (energy)) : 1.0;
            for (int k = 1; k <= kMaxHarmonic; ++k)
            {
                cosAmp[k] *= rmsGain;
                sinAmp[k] *= rmsGain;
            }

            std::memset (acc, 0, sizeof (acc));
            int prevMaxH = 0;
            for (int mip = kNumWtMips - 1; mip >= 0; --mip)
            {
                const int maxH = 1 << (kNumWtMips - 1 - mip); // 1, 2, 4, ..., 256
                for (int k = prevMaxH + 1; k <= maxH; ++k)
                {
                    const double ca = cosAmp[k];
                    const double sa = sinAmp[k];
                    if (std::fabs (ca) < 1.0e-12 && std::fabs (sa) < 1.0e-12)
                        continue;
                    for (int n = 0; n < kWtFrameSize; ++n)
                    {
                        const int idx = (k * n) & kMask;
                        acc[n] += ca * cosLut[idx] + sa * sinLut[idx];
                    }
                }
                prevMaxH = maxH;
                for (int n = 0; n < kWtFrameSize; ++n)
                    gTables[t][f][mip][n] = static_cast<float> (clampSafe (acc[n], 1.2));
                gTables[t][f][mip][kWtFrameSize] = gTables[t][f][mip][0];
            }
        }
    }
    gInitialised.store (true, std::memory_order_release);
}

} // namespace

void initWavetables() noexcept
{
    std::call_once (gInitFlag, buildAllWavetables);
}

bool wavetablesInitialised() noexcept
{
    return gInitialised.load (std::memory_order_acquire);
}

int selectWtMip (double hz, double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double f = clampPitch (hz, sr);
    const double maxAllowedH = (0.45 * sr) / f;
    // Mip m has max harmonic 2^(8 - m). Pick the lowest mip with 2^(8 - m) <= maxAllowedH.
    for (int mip = 0; mip < kNumWtMips - 1; ++mip)
    {
        const int h = 1 << (kNumWtMips - 1 - mip);
        if (static_cast<double> (h) <= maxAllowedH)
            return mip;
    }
    return kNumWtMips - 1;
}

float wavetableSample (int table, int frame, int mip, int index) noexcept
{
    const int t = clampSlot (table, kNumWavetables);
    const int f = clampSlot (frame, kNumWtFrames);
    const int m = clampSlot (mip, kNumWtMips);
    const int i = index & kMask;
    return gTables[t][f][m][i];
}

double WtOscState::process (double hz, double table, double pos, double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double f = clampPitch (hz, sr);
    const int tbl = static_cast<int> (std::floor (sanitizeFloat (table) + 0.5));
    const double targetPos = clamp01 (pos);
    if (! posInit)
    {
        smoothPos = targetPos;
        posInit = true;
    }
    else
    {
        smoothPos += (targetPos - smoothPos) * 0.015;
    }

    if (tbl >= 8 && tbl < 8 + kMaxLoadedWavetables)
    {
        const LoadedWavetable* wt = getLoadedWavetable (tbl);
        if (wt != nullptr)
        {
            const double out = sampleLoadedWavetable (wt, f, phase, smoothPos, sr);
            phase += f / sr;
            phase -= std::floor (phase);
            return clampSafe (out);
        }
    }

    const int safeTbl = (tbl >= 0 && tbl < kNumWavetables) ? tbl : 0;
    const int mip = selectWtMip (f, sr);
    const double fScaled = smoothPos * static_cast<double> (kNumWtFrames - 1);
    int f0 = static_cast<int> (fScaled);
    if (f0 >= kNumWtFrames - 1)
        f0 = kNumWtFrames - 2;
    if (f0 < 0)
        f0 = 0;
    const double fFrac = fScaled - static_cast<double> (f0);

    const double sScaled = phase * static_cast<double> (kWtFrameSize);
    const int s0 = static_cast<int> (sScaled) & kMask;
    const double sFrac = sScaled - std::floor (sScaled);

    const float* row0 = gTables[safeTbl][f0][mip];
    const float* row1 = gTables[safeTbl][f0 + 1][mip];
    const double v0 = row0[s0] + sFrac * (row0[s0 + 1] - row0[s0]);
    const double v1 = row1[s0] + sFrac * (row1[s0 + 1] - row1[s0]);
    const double out = v0 + fFrac * (v1 - v0);

    phase += f / sr;
    phase -= std::floor (phase);
    return clampSafe (out);
}

void UnisonOscState::updateControl (double f0, double table, double voices, double detune,
                                    double spread, double sampleRate) noexcept
{
    if (! sameBitsD (sampleRate, coeffRate))
    {
        coeffRate = sampleRate;
        coeff = 1.0 - std::exp (-static_cast<double> (kUnisonControlInterval) / (0.008 * sampleRate));
    }

    const double voicesIn = std::clamp (sanitizeFloat (voices), 1.0, static_cast<double> (kMaxUnisonVoices));
    const int targetVoices = static_cast<int> (voicesIn + 0.5);
    const double targetDetune = clamp01 (detune);
    const double targetSpread = clamp01 (spread);

    bool changed = ! initialised;
    auto targetTOf = [targetVoices] (int v) noexcept
    {
        return targetVoices > 1 ? 2.0 * static_cast<double> (v) / static_cast<double> (targetVoices - 1) - 1.0 : 0.0;
    };
    // Moves x toward target, landing on it once the rest can't be heard.
    auto glide = [this, &changed] (double& x, double target, double snap) noexcept
    {
        if (sameBitsD (x, target))
            return;
        x += (target - x) * coeff;
        if (std::fabs (target - x) < snap)
            x = target;
        changed = true;
    };

    if (! initialised)
    {
        smoothDetune = targetDetune;
        smoothSpread = targetSpread;
        for (int v = 0; v < kMaxUnisonVoices; ++v)
        {
            voiceGains[v] = v < targetVoices ? 1.0 : 0.0;
            smoothT[v] = v < targetVoices ? targetTOf (v) : (targetVoices > 1 ? 1.0 : 0.0);
        }
    }
    else
    {
        const bool detuneMoves = ! sameBitsD (smoothDetune, targetDetune);
        glide (smoothDetune, targetDetune, 1.0e-6);
        glide (smoothSpread, targetSpread, 1.0e-6);
        if (detuneMoves)
            detuneCents = 50.0 * std::pow (smoothDetune, 1.75);
        for (int v = 0; v < kMaxUnisonVoices; ++v)
        {
            const bool on = v < targetVoices;
            glide (voiceGains[v], on ? 1.0 : 0.0, on ? 1.0e-6 : 1.0e-4);
            // A voice being faded out stays where it is in the stereo field.
            if (on)
                glide (smoothT[v], targetTOf (v), 1.0e-6);
        }
    }
    if (! initialised)
        detuneCents = 50.0 * std::pow (smoothDetune, 1.75);

    // The table: a loaded one once published, else the built-in fallback.
    const double tableIn = sanitizeFloat (table);
    const int tbl = (tableIn >= -0.5 && tableIn < 1000.0) ? static_cast<int> (tableIn + 0.5) : 0;
    const LoadedWavetable* loaded = (tbl >= 8 && tbl < 8 + kMaxLoadedWavetables) ? getLoadedWavetable (tbl) : nullptr;
    if (loaded != nullptr && (loaded->numFrames <= 0 || loaded->data.empty()))
        loaded = nullptr;
    const int builtin = loaded == nullptr ? ((tbl >= 0 && tbl < kNumWavetables) ? tbl : 0) : 0;
    const bool tableChanged = loaded != lastLoaded || builtin != lastBuiltin;
    lastLoaded = loaded;
    lastBuiltin = builtin;
    frameCount = loaded != nullptr ? loaded->numFrames : kNumWtFrames;
    frameStride = loaded != nullptr ? kLoadedWtFrameStride : kNumWtMips * (kWtFrameSize + 1);

    if (changed)
    {
        double sumGainSq = 0.0;
        for (int v = 0; v < kMaxUnisonVoices; ++v)
            sumGainSq += voiceGains[v] * voiceGains[v];
        const double norm = sumGainSq > 1.0e-6 ? (1.0 / std::sqrt (sumGainSq)) : 1.0;
        const double rampScale = initialised ? 1.0 / static_cast<double> (kUnisonControlInterval) : 0.0;
        for (int v = 0; v < kMaxUnisonVoices; ++v)
        {
            const double g = voiceGains[v];
            if (g <= 0.0 && std::fabs (gainL[v]) + std::fabs (gainR[v]) < 1.0e-9)
            {
                gainL[v] = gainR[v] = stepL[v] = stepR[v] = targetL[v] = targetR[v] = 0.0;
                continue;
            }
            targetL[v] = targetR[v] = 0.0;
            if (g > 0.0)
            {
                const double t = smoothT[v];
                ratios[v] = std::exp (t * detuneCents * (0.6931471805599453 / 1200.0));
                // Equal-power pan, theta in [0, pi/2]. At spread 0, L == R.
                constexpr double kQuarterPi = 0.7853981633974483;
                constexpr double kSqrt2 = 1.4142135623730951;
                const double theta = kQuarterPi * (1.0 + t * smoothSpread);
                const double effGain = g * norm * kSqrt2;
                targetL[v] = effGain * std::cos (theta);
                targetR[v] = effGain * std::sin (theta);
            }
            if (initialised)
            {
                stepL[v] = (targetL[v] - gainL[v]) * rampScale;
                stepR[v] = (targetR[v] - gainR[v]) * rampScale;
            }
            else
            {
                gainL[v] = targetL[v];
                gainR[v] = targetR[v];
                stepL[v] = stepR[v] = 0.0;
            }
        }
        ramping = initialised;
    }
    else if (ramping)
    {
        for (int v = 0; v < kMaxUnisonVoices; ++v)
        {
            gainL[v] = targetL[v];
            gainR[v] = targetR[v];
            stepL[v] = stepR[v] = 0.0;
        }
        ramping = false;
    }

    int active = 0;
    for (int v = 0; v < kMaxUnisonVoices; ++v)
        if (voiceGains[v] > 0.0 || gainL[v] != 0.0 || gainR[v] != 0.0)
            active = v + 1;
    const int previouslyActive = activeVoices;
    activeVoices = active;

    // Silent voices: phase only, in one step for the whole interval.
    const double blockInc = static_cast<double> (kUnisonControlInterval) * f0 / sampleRate;
    for (int v = active; v < kMaxUnisonVoices; ++v)
    {
        const double p = phases[v] + blockInc;
        phases[v] = p - static_cast<double> (static_cast<int64_t> (p));
    }

    if (changed || tableChanged || active != previouslyActive || ! sameBitsD (f0, lastF0) || ! initialised)
    {
        lastF0 = f0;
        for (int v = 0; v < active; ++v)
        {
            const double fv = f0 * ratios[v];
            if (loaded != nullptr)
            {
                const int mip = selectLoadedWtMip (fv, sampleRate);
                rowBase[v] = loaded->data.data() + loadedWtMipOffset (mip);
                rowSize[v] = static_cast<double> (kBaseWtFrameSize >> mip);
                rowMask[v] = (kBaseWtFrameSize >> mip) - 1;
            }
            else
            {
                rowBase[v] = gTables[builtin][0][selectWtMip (fv, sampleRate)];
                rowSize[v] = static_cast<double> (kWtFrameSize);
                rowMask[v] = kMask;
            }
        }
    }
    initialised = true;
}

double UnisonOscState::process (double hz, double table, double pos, double voices,
                                double detune, double spread, double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double f0 = clampPitch (hz, sr);
    const double targetPos = clamp01 (pos);
    if (! initialised)
        smoothPos = targetPos;
    if (--countdown < 0)
    {
        updateControl (f0, table, voices, detune, spread, sr);
        countdown = kUnisonControlInterval - 1;
    }
    smoothPos += (targetPos - smoothPos) * 0.015;

    const int frames = frameCount;
    const double fScaled = smoothPos * static_cast<double> (frames - 1);
    int frame = static_cast<int> (fScaled);
    if (frame > frames - 2)
        frame = frames - 2;
    if (frame < 0)
        frame = 0;
    const double fFrac = frames > 1 ? fScaled - static_cast<double> (frame) : 0.0;
    const int rowOffset = frame * frameStride;
    const int nextFrame = frames > 1 ? frameStride : 0;

    const double baseInc = f0 / sr;
    double sumL = 0.0, sumR = 0.0;
    const int active = activeVoices;
    for (int v = 0; v < active; ++v)
    {
        const double p = phases[v];
        const double sScaled = p * rowSize[v];
        const int s0 = static_cast<int> (sScaled);
        const double sFrac = sScaled - static_cast<double> (s0);
        const int s = s0 & rowMask[v];
        const float* row0 = rowBase[v] + rowOffset;
        const float* row1 = row0 + nextFrame;
        const double v0 = row0[s] + sFrac * (row0[s + 1] - row0[s]);
        const double v1 = row1[s] + sFrac * (row1[s + 1] - row1[s]);
        const double samp = v0 + fFrac * (v1 - v0);

        sumL += samp * gainL[v];
        sumR += samp * gainR[v];
        gainL[v] += stepL[v];
        gainR[v] += stepR[v];

        double inc = baseInc * ratios[v];
        if (inc > 0.45)
            inc = 0.45;
        const double next = p + inc;
        phases[v] = next >= 1.0 ? next - 1.0 : next;
    }

    lastR = static_cast<float> (clampSafe (sumR));
    return clampSafe (sumL);
}

double FmOscState::process (double hz, double ratio, double index, double feedback,
                            double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double fc = clampPitch (hz, sr);
    const double r = mapRatio (ratio);
    const double fm = std::fmin (fc * r, 0.48 * sr);
    const double nyqScale = fc * r > 0.45 * sr ? std::fmax (0.0, 1.0 - (fc * r - 0.45 * sr) / (0.5 * sr)) : 1.0;

    const double fb = clamp01 (feedback) * 2.2 * nyqScale;
    const double idx = clamp01 (index) * 10.0 * nyqScale;

    const double fbMod = 0.5 * (prevMod1 + prevMod2);
    const double mod = std::sin (kTwoPi * mPhase + fb * fbMod);
    prevMod2 = prevMod1;
    prevMod1 = mod;

    const double out = std::sin (kTwoPi * cPhase + idx * mod);

    cPhase += fc / sr;
    cPhase -= std::floor (cPhase);
    mPhase += fm / sr;
    mPhase -= std::floor (mPhase);
    return clampSafe (out * 0.9);
}

double SyncOscState::process (double hz, double ratio, double shape, double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double fMaster = clampPitch (hz, sr);
    const double r = std::exp2 (4.0 * clamp01 (ratio)); // 1..16x
    const double fSlave = std::fmin (fMaster * r, 0.48 * sr);
    const double sh = clamp01 (shape);

    const double dtM = fMaster / sr;
    const double dtS = fSlave / sr;

    masterPhase += dtM;
    bool synced = false;
    double tSync = 0.0;
    double slaveBeforeSync = 0.0;

    if (masterPhase >= 1.0)
    {
        masterPhase -= std::floor (masterPhase);
        tSync = dtM > 1.0e-12 ? (masterPhase / dtM) : 0.0;
        if (tSync > 1.0) tSync = 1.0;
        slaveBeforeSync = slavePhase + dtS * (1.0 - tSync);
        slaveBeforeSync -= std::floor (slaveBeforeSync);
        slavePhase = dtS * tSync;
        slavePhase -= std::floor (slavePhase);
        synced = true;
    }
    else
    {
        slavePhase += dtS;
        slavePhase -= std::floor (slavePhase);
    }

    auto rawWave = [sh] (double p) noexcept -> double
    {
        const double saw = 2.0 * p - 1.0;
        const double sq = p < 0.5 ? 1.0 : -1.0;
        return (1.0 - sh) * saw + sh * sq;
    };

    double out = rawWave (slavePhase);
    // Standard PolyBLEP on slave discontinuities:
    out -= (1.0 - sh) * polyBlep (slavePhase, dtS);
    if (sh > 0.0)
    {
        out += sh * polyBlep (slavePhase, dtS);
        const double pHalf = slavePhase < 0.5 ? (slavePhase + 0.5) : (slavePhase - 0.5);
        out -= sh * polyBlep (pHalf, dtS);
    }
    // PolyBLEP on hard-sync master reset discontinuity:
    if (synced && tSync < 1.0)
    {
        const double jump = rawWave (0.0) - rawWave (slaveBeforeSync);
        const double u = tSync;
        const double blep = (u + u - u * u - 1.0);
        out -= 0.5 * jump * blep;
    }
    return clampSafe (out * 0.85);
}

double ModLfoState::process (double currentBeat, double beatsArg, double shapeArg) noexcept
{
    const double bRaw = sanitizeFloat (beatsArg);
    const double period = std::fmax (1.0e-4, std::fmin (64.0, bRaw > 0.0 ? bRaw : 1.0));
    const double relBeat = sanitizeFloat (currentBeat) - anchorBeat;
    const double cycles = relBeat / period;
    const double cycFloor = std::floor (cycles);
    const double p = cycles - cycFloor;
    const int64_t cycIdx = static_cast<int64_t> (cycFloor);

    if (cycIdx != lastCycle)
    {
        lastCycle = cycIdx;
        // Deterministic S&H value per cycle and seed:
        uint32_t h = rngState ^ static_cast<uint32_t> (cycIdx * 0x9E3779B9u);
        h ^= h << 13;
        h ^= h >> 17;
        h ^= h << 5;
        shValue = static_cast<double> (h & 0x00FFFFFFu) / static_cast<double> (0x00FFFFFFu);
    }

    const double vSine = 0.5 - 0.5 * std::cos (kTwoPi * p);
    const double vTri = 1.0 - std::fabs (2.0 * p - 1.0);
    const double vSaw = p;
    const double vSq = p < 0.5 ? 1.0 : 0.0;
    const double vSh = shValue;

    const double vals[5] = { vSine, vTri, vSaw, vSq, vSh };
    const double sScaled = clamp01 (shapeArg) * 4.0;
    int s0 = static_cast<int> (sScaled);
    if (s0 >= 4)
        s0 = 3;
    if (s0 < 0)
        s0 = 0;
    const double sFrac = sScaled - static_cast<double> (s0);
    const double out = vals[s0] + sFrac * (vals[s0 + 1] - vals[s0]);
    return clamp01 (out);
}

double ModCurveState::process (void* vm, double beat, double beatsIn, double baseIn,
                               double pointsIn, double sampleRate, bool restarted) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    if (! sameBitsD (sr, decayRate))
    {
        decayRate = sr;
        decay = std::exp (-1.0 / (0.001 * sr));
    }
    const auto holdOff = [this] () noexcept
    {
        jumpOffset *= decay;
        return clamp01 (jumpOffset);
    };

    const double bRaw = isFiniteBitsD (beatsIn) ? beatsIn : 1.0;
    const double beats = bRaw > 1.0e-4 ? bRaw : 1.0;
    const double baseRaw = isFiniteBitsD (baseIn) ? baseIn : 0.0;
    const double pointsRaw = isFiniteBitsD (pointsIn) ? pointsIn : 2.0;
    const int numPoints = static_cast<int> (std::clamp (pointsRaw, 2.0, 32.0) + 0.5);

    // Range first: rounding a huge value to int would be undefined.
    if (vm == nullptr || baseRaw < -0.5 || baseRaw > 65536.0)
        return holdOff();
    const int base = static_cast<int> (std::floor (baseRaw + 0.5));
    if (base < 0 || base + 3 * numPoints > 65536)
        return holdOff();

    int valid = 0;
    const EEL_F* ram = NSEEL_VM_getramptr_noalloc (static_cast<NSEEL_VMCTX> (vm),
                                                   static_cast<unsigned int> (base), &valid);
    if (ram == nullptr || valid < 3 * numPoints)
        return holdOff();

    const double rawBeat = isFiniteBitsD (beat) ? beat : 0.0;
    const double u = clamp01 ((rawBeat - beats * std::floor (rawBeat / beats)) / beats);
    const bool wrapped = restarted || (initialised && u < prevPhase - 0.5);
    prevPhase = u;

    // Find the segment: from last sample's, or from point 0 if the position
    // has gone back past it. Ascending x is enforced by the running maximum.
    int i = segment;
    double prefix = prefixX;
    double x0 = 0.0;
    bool rescan = i > numPoints - 2;
    if (! rescan)
    {
        const double rx = ram[3 * i];
        if (! isFiniteBitsD (rx))
            return holdOff();
        x0 = std::fmax (prefix, clamp01 (rx));
        rescan = u < x0;
    }
    if (rescan)
    {
        const double rx = ram[0];
        if (! isFiniteBitsD (rx))
            return holdOff();
        i = 0;
        prefix = 0.0;
        x0 = clamp01 (rx);
    }

    double rawY = 0.0;
    bool isVerticalJump = false;
    if (i == 0 && u <= x0)
    {
        const double ry = ram[1];
        if (! isFiniteBitsD (ry))
            return holdOff();
        rawY = clamp01 (ry);
    }
    else
    {
        double x1 = x0;
        for (;;)
        {
            const double rx = ram[3 * (i + 1)];
            if (! isFiniteBitsD (rx))
                return holdOff();
            x1 = std::fmax (x0, clamp01 (rx));
            if (u <= x1 || i == numPoints - 2)
                break;
            if (x1 - x0 <= 1.0e-7)
                isVerticalJump = true;  // walked over it: u rarely lands exactly on one
            prefix = x0;
            x0 = x1;
            ++i;
        }

        const double ry0 = ram[3 * i + 1];
        const double ry1 = ram[3 * (i + 1) + 1];
        const double rb = ram[3 * i + 2];
        if (! isFiniteBitsD (ry0) || ! isFiniteBitsD (ry1) || ! isFiniteBitsD (rb))
            return holdOff();
        const double y0 = clamp01 (ry0);
        const double y1 = clamp01 (ry1);

        const double span = x1 - x0;
        if (span <= 1.0e-7)
        {
            rawY = y1;
            isVerticalJump = true;
        }
        else if (u >= x1)
        {
            rawY = y1;
        }
        else
        {
            const double t = clamp01 ((u - x0) / span);
            const double b = std::clamp (rb, -8.0, 8.0);
            double w = t;
            if (std::fabs (b) >= 0.01)
            {
                if (! sameBitsD (b, cachedBend))
                {
                    cachedBend = b;
                    bendScale = 1.0 / (std::exp (b) - 1.0);
                }
                w = clamp01 ((std::exp (b * t) - 1.0) * bendScale);
            }
            rawY = y0 + w * (y1 - y0);
        }
    }
    segment = i;
    prefixX = prefix;

    if (! initialised)
    {
        initialised = true;
        prevRawY = rawY;
        jumpOffset = 0.0;
        return clamp01 (rawY);
    }

    if (isVerticalJump || wrapped)
    {
        const double step = rawY - prevRawY;
        if (std::fabs (step) > 1.0e-5)
            jumpOffset -= step;
    }

    jumpOffset *= decay;
    prevRawY = rawY;
    return clamp01 (rawY + jumpOffset);
}

double ModCurveNoteState::process (void* vm, double beat, double beatsIn, double baseIn,
                                   double pointsIn, double sampleRate) noexcept
{
    const double now = isFiniteBitsD (beat) ? beat : lastBeat;
    const bool restarted = restartPending;
    if (restarted)
    {
        restartPending = false;
        elapsed = 0.0;
        lastStep = 0.0;
    }
    else
    {
        const double d = now - lastBeat;
        if (d >= 0.0 && d <= kMaxBeatStep)
            lastStep = d;
        elapsed += lastStep;
    }
    lastBeat = now;
    return curve.process (vm, elapsed, beatsIn, baseIn, pointsIn, sampleRate, restarted);
}

} // namespace vibesynth
