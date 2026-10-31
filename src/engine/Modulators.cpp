#include "engine/Modulators.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/Utilities.h"

namespace vibesynth {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

inline double onePoleCoeff (double seconds, double sampleRate) noexcept
{
    return 1.0 - std::exp (-1.0 / std::max (1.0, seconds * sampleRate));
}

inline bool sameModNumber (double a, double b) noexcept
{
    return std::memcmp (&a, &b, sizeof (double)) == 0;
}

struct SyncDivEntry
{
    LfoSyncDiv div;
    const char* id;
    const char* label;
    double beats; // in quarter-note beats (4 beats = 1 bar in 4/4)
};

constexpr SyncDivEntry kSyncDivs[] = {
    { LfoSyncDiv::Div1_32T, "1/32T", "1/32T", 0.125 * (2.0 / 3.0) },
    { LfoSyncDiv::Div1_32,  "1/32",  "1/32",  0.125 },
    { LfoSyncDiv::Div1_32D, "1/32D", "1/32D", 0.125 * 1.5 },
    { LfoSyncDiv::Div1_16T, "1/16T", "1/16T", 0.25 * (2.0 / 3.0) },
    { LfoSyncDiv::Div1_16,  "1/16",  "1/16",  0.25 },
    { LfoSyncDiv::Div1_16D, "1/16D", "1/16D", 0.25 * 1.5 },
    { LfoSyncDiv::Div1_8T,  "1/8T",  "1/8T",  0.5 * (2.0 / 3.0) },
    { LfoSyncDiv::Div1_8,   "1/8",   "1/8",   0.5 },
    { LfoSyncDiv::Div1_8D,  "1/8D",  "1/8D",  0.5 * 1.5 },
    { LfoSyncDiv::Div1_4T,  "1/4T",  "1/4T",  1.0 * (2.0 / 3.0) },
    { LfoSyncDiv::Div1_4,   "1/4",   "1/4",   1.0 },
    { LfoSyncDiv::Div1_4D,  "1/4D",  "1/4D",  1.5 },
    { LfoSyncDiv::Div1_2T,  "1/2T",  "1/2T",  2.0 * (2.0 / 3.0) },
    { LfoSyncDiv::Div1_2,   "1/2",   "1/2",   2.0 },
    { LfoSyncDiv::Div1_2D,  "1/2D",  "1/2D",  3.0 },
    { LfoSyncDiv::Div1_1T,  "1/1T",  "1 bar T",  4.0 * (2.0 / 3.0) },
    { LfoSyncDiv::Div1_1,   "1/1",   "1 bar",    4.0 },
    { LfoSyncDiv::Div1_1D,  "1/1D",  "1 bar D",  6.0 },
    { LfoSyncDiv::Div2_1T,  "2/1T",  "2 bars T", 8.0 * (2.0 / 3.0) },
    { LfoSyncDiv::Div2_1,   "2/1",   "2 bars",   8.0 },
    { LfoSyncDiv::Div2_1D,  "2/1D",  "2 bars D", 12.0 },
    { LfoSyncDiv::Div4_1T,  "4/1T",  "4 bars T", 16.0 * (2.0 / 3.0) },
    { LfoSyncDiv::Div4_1,   "4/1",   "4 bars",   16.0 },
    { LfoSyncDiv::Div4_1D,  "4/1D",  "4 bars D", 24.0 },
};

// Deterministic cycle hash in [-1.0, +1.0] for S&H and Smooth Random.
double cycleRandomBipolar (int64_t cycleIndex, uint32_t seed) noexcept
{
    uint64_t x = static_cast<uint64_t> (cycleIndex) * 0x9E3779B97F4A7C15ULL
               + (static_cast<uint64_t> (seed) << 1) + 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x = x ^ (x >> 31);
    const double u01 = static_cast<double> (x & 0x1FFFFFFFFFFFFFULL)
                     * (1.0 / static_cast<double> (0x1FFFFFFFFFFFFFULL));
    return std::clamp (u01 * 2.0 - 1.0, -1.0, 1.0);
}

} // namespace

const char* modSourceId (ModSource s) noexcept
{
    switch (s)
    {
        case ModSource::Lfo1: return "lfo1";
        case ModSource::Lfo2: return "lfo2";
        case ModSource::Env1: return "env1";
        case ModSource::Env2: return "env2";
    }
    return "lfo1";
}

const char* modSourceLabel (ModSource s) noexcept
{
    switch (s)
    {
        case ModSource::Lfo1: return "LFO 1";
        case ModSource::Lfo2: return "LFO 2";
        case ModSource::Env1: return "ENV 1";
        case ModSource::Env2: return "ENV 2";
    }
    return "LFO 1";
}

bool parseModSourceId (const std::string& s, ModSource& out) noexcept
{
    if (s == "lfo1") { out = ModSource::Lfo1; return true; }
    if (s == "lfo2") { out = ModSource::Lfo2; return true; }
    if (s == "env1") { out = ModSource::Env1; return true; }
    if (s == "env2") { out = ModSource::Env2; return true; }
    return false;
}

const char* lfoShapeId (LfoShape s) noexcept
{
    switch (s)
    {
        case LfoShape::Sine:          return "sine";
        case LfoShape::Triangle:      return "triangle";
        case LfoShape::SawUp:         return "saw_up";
        case LfoShape::SawDown:       return "saw_down";
        case LfoShape::Square:        return "square";
        case LfoShape::SampleAndHold: return "sh";
        case LfoShape::SmoothRandom:  return "smooth_random";
        case LfoShape::Count:         break;
    }
    return "sine";
}

const char* lfoShapeLabel (LfoShape s) noexcept
{
    switch (s)
    {
        case LfoShape::Sine:          return "Sine";
        case LfoShape::Triangle:      return "Triangle";
        case LfoShape::SawUp:         return "Saw Up";
        case LfoShape::SawDown:       return "Saw Down";
        case LfoShape::Square:        return "Square";
        case LfoShape::SampleAndHold: return "S&H";
        case LfoShape::SmoothRandom:  return "Smooth Rnd";
        case LfoShape::Count:         break;
    }
    return "Sine";
}

bool parseLfoShapeId (const std::string& s, LfoShape& out) noexcept
{
    for (int i = 0; i < static_cast<int> (LfoShape::Count); ++i)
    {
        const auto sh = static_cast<LfoShape> (i);
        if (s == lfoShapeId (sh))
        {
            out = sh;
            return true;
        }
    }
    if (s == "sample_and_hold" || s == "s&h")
    {
        out = LfoShape::SampleAndHold;
        return true;
    }
    return false;
}

const char* lfoSyncDivId (LfoSyncDiv d) noexcept
{
    const int idx = static_cast<int> (d);
    if (idx >= 0 && idx < static_cast<int> (LfoSyncDiv::Count))
        return kSyncDivs[idx].id;
    return "1/4";
}

const char* lfoSyncDivLabel (LfoSyncDiv d) noexcept
{
    const int idx = static_cast<int> (d);
    if (idx >= 0 && idx < static_cast<int> (LfoSyncDiv::Count))
        return kSyncDivs[idx].label;
    return "1/4";
}

double lfoSyncDivBeats (LfoSyncDiv d) noexcept
{
    const int idx = static_cast<int> (d);
    if (idx >= 0 && idx < static_cast<int> (LfoSyncDiv::Count))
        return kSyncDivs[idx].beats;
    return 1.0;
}

bool parseLfoSyncDivId (const std::string& s, LfoSyncDiv& out) noexcept
{
    for (const auto& e : kSyncDivs)
    {
        if (s == e.id || s == e.label)
        {
            out = e.div;
            return true;
        }
    }
    return false;
}

bool ModLfoSpec::isDefault() const noexcept
{
    const ModLfoSpec def {};
    return *this == def;
}

bool ModLfoSpec::operator== (const ModLfoSpec& o) const noexcept
{
    return shape == o.shape && sync == o.sync && sameModNumber (rateHz, o.rateHz)
        && syncDiv == o.syncDiv && retrigger == o.retrigger;
}

bool ModEnvSpec::isDefault() const noexcept
{
    const ModEnvSpec def {};
    return *this == def;
}

bool ModEnvSpec::operator== (const ModEnvSpec& o) const noexcept
{
    return sameModNumber (attack, o.attack) && sameModNumber (decay, o.decay)
        && sameModNumber (sustain, o.sustain) && sameModNumber (release, o.release);
}

bool ModAssignment::operator== (const ModAssignment& o) const noexcept
{
    return source == o.source && macro == o.macro && node == o.node
        && arg == o.arg && sameModNumber (depth, o.depth);
}

// ---------------------------------------------------------------------------
// LfoVoiceState

void LfoVoiceState::prepare (double sampleRate, int lfoIndex, int voiceIndex) noexcept
{
    sampleRate_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
    invSampleRate_ = 1.0 / sampleRate_;
    smoothCoeff_ = onePoleCoeff (0.0005, sampleRate_);
    baseSeed_ = static_cast<uint32_t> (0x13579BDFu + lfoIndex * 1013u + voiceIndex * 7919u);
    noteSeed_ = baseSeed_;
    reset();
}

void LfoVoiceState::reset() noexcept
{
    phase_ = 0.0;
    cycleCount_ = 0;
    smoothed_ = 0.0;
    firstSample_ = true;
}

void LfoVoiceState::noteOn (int midiNote) noexcept
{
    phase_ = 0.0;
    cycleCount_ = 0;
    noteSeed_ = baseSeed_ ^ static_cast<uint32_t> ((midiNote & 0x7F) * 2654435761u);
}

double LfoVoiceState::evalShape (LfoShape shape, double phase, int64_t cycleIndex, uint32_t seed) noexcept
{
    const double p = std::clamp (phase, 0.0, 0.999999999);
    switch (shape)
    {
        case LfoShape::Sine:
            return std::clamp (fastSin (kTwoPi * p), -1.0, 1.0);

        case LfoShape::Triangle:
            if (p < 0.25)
                return 4.0 * p;
            if (p < 0.75)
                return 2.0 - 4.0 * p;
            return 4.0 * p - 4.0;

        case LfoShape::SawUp:
            return 2.0 * p - 1.0;

        case LfoShape::SawDown:
            return 1.0 - 2.0 * p;

        case LfoShape::Square:
            return p < 0.5 ? 1.0 : -1.0;

        case LfoShape::SampleAndHold:
            return cycleRandomBipolar (cycleIndex, seed);

        case LfoShape::SmoothRandom:
        {
            const double v0 = cycleRandomBipolar (cycleIndex, seed);
            const double v1 = cycleRandomBipolar (cycleIndex + 1, seed);
            const double s = p * p * (3.0 - 2.0 * p);
            return v0 + (v1 - v0) * s;
        }

        case LfoShape::Count:
            break;
    }
    return 0.0;
}

double LfoVoiceState::step (const ModLfoSpec& spec, double bpmIn, double beatIn, double& phaseOut) noexcept
{
    const double bpm = isFiniteBitsD (bpmIn) ? std::clamp (bpmIn, 20.0, 400.0) : 120.0;
    const double beat = isFiniteBitsD (beatIn) ? beatIn : 0.0;

    double p = 0.0;
    int64_t cyc = 0;
    uint32_t seed = noteSeed_;

    if (! spec.retrigger)
    {
        // Free-running: derive phase directly from the host's beat position so
        // every active voice agrees on phase and random cycle values.
        double totalCycles = 0.0;
        if (spec.sync)
        {
            const double periodBeats = std::max (1.0e-4, lfoSyncDivBeats (spec.syncDiv));
            totalCycles = beat / periodBeats;
        }
        else
        {
            const double rate = isFiniteBitsD (spec.rateHz) ? std::clamp (spec.rateHz, 0.01, 50.0) : 2.0;
            const double seconds = beat * (60.0 / bpm);
            totalCycles = seconds * rate;
        }
        const double fl = std::floor (totalCycles);
        cyc = static_cast<int64_t> (fl);
        p = totalCycles - fl;
        seed = 0x5A17C3E9u ^ (baseSeed_ & 0x0FFFu); // shared across voices for the same LFO slot
        phase_ = p;
        cycleCount_ = cyc;
    }
    else
    {
        p = phase_;
        cyc = cycleCount_;
        double inc = 0.0;
        if (spec.sync)
        {
            const double periodBeats = std::max (1.0e-4, lfoSyncDivBeats (spec.syncDiv));
            const double beatsPerSample = (bpm / 60.0) * invSampleRate_;
            inc = beatsPerSample / periodBeats;
        }
        else
        {
            const double rate = isFiniteBitsD (spec.rateHz) ? std::clamp (spec.rateHz, 0.01, 50.0) : 2.0;
            inc = rate * invSampleRate_;
        }
        phase_ += std::clamp (inc, 0.0, 0.5);
        while (phase_ >= 1.0)
        {
            phase_ -= 1.0;
            ++cycleCount_;
        }
    }

    phaseOut = std::clamp (p, 0.0, 1.0);
    const double raw = evalShape (spec.shape, phaseOut, cyc, seed);

    if (firstSample_)
    {
        smoothed_ = raw;
        firstSample_ = false;
    }
    else
    {
        smoothed_ += smoothCoeff_ * (raw - smoothed_);
    }

    if (! isFiniteBitsD (smoothed_))
        smoothed_ = 0.0;
    smoothed_ = std::clamp (smoothed_, -1.0, 1.0);
    return smoothed_;
}

double LfoVoiceState::advance (const ModLfoSpec& spec, double bpmIn, double beatIn, int64_t n, double& phaseOut,
                               int64_t& cycleOut) noexcept
{
    const double bpm = isFiniteBitsD (bpmIn) ? std::clamp (bpmIn, 20.0, 400.0) : 120.0;
    const double beat = isFiniteBitsD (beatIn) ? beatIn : 0.0;

    double p = 0.0;
    int64_t cyc = 0;
    uint32_t seed = noteSeed_;

    if (! spec.retrigger)
    {
        // Free-running: the phase is a function of the host beat (as step()).
        double totalCycles = 0.0;
        if (spec.sync)
        {
            const double periodBeats = std::max (1.0e-4, lfoSyncDivBeats (spec.syncDiv));
            totalCycles = beat / periodBeats;
        }
        else
        {
            const double rate = isFiniteBitsD (spec.rateHz) ? std::clamp (spec.rateHz, 0.01, 50.0) : 2.0;
            totalCycles = beat * (60.0 / bpm) * rate;
        }
        totalCycles = std::clamp (totalCycles, -1.0e15, 1.0e15);
        const double fl = std::floor (totalCycles);
        cyc = static_cast<int64_t> (fl);
        p = totalCycles - fl;
        seed = 0x5A17C3E9u ^ (baseSeed_ & 0x0FFFu);
        phase_ = p;
        cycleCount_ = cyc;
    }
    else
    {
        if (n > 0)
        {
            double inc = 0.0;
            if (spec.sync)
            {
                const double periodBeats = std::max (1.0e-4, lfoSyncDivBeats (spec.syncDiv));
                inc = (bpm / 60.0) * invSampleRate_ / periodBeats;
            }
            else
            {
                const double rate = isFiniteBitsD (spec.rateHz) ? std::clamp (spec.rateHz, 0.01, 50.0) : 2.0;
                inc = rate * invSampleRate_;
            }
            const double adv = std::clamp (inc, 0.0, 0.5) * static_cast<double> (n);
            const double whole = std::floor (adv);
            phase_ += adv - whole;
            cycleCount_ += static_cast<int64_t> (whole);
            if (phase_ >= 1.0)
            {
                phase_ -= 1.0;
                ++cycleCount_;
            }
        }
        p = phase_;
        cyc = cycleCount_;
    }

    phaseOut = std::clamp (p, 0.0, 1.0);
    cycleOut = cyc;
    const double raw = evalShape (spec.shape, phaseOut, cyc, seed);
    return isFiniteBitsD (raw) ? std::clamp (raw, -1.0, 1.0) : 0.0;
}

// ---------------------------------------------------------------------------
// AdsrVoiceState

void AdsrVoiceState::prepare (double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
    smoothCoeff_ = onePoleCoeff (0.0005, sampleRate_);
    reset();
}

void AdsrVoiceState::reset() noexcept
{
    stage_ = Stage::Idle;
    env_ = 0.0;
    smoothed_ = 0.0;
    releaseStart_ = 0.0;
    prevGate_ = 0.0;
}

void AdsrVoiceState::noteOn() noexcept
{
    stage_ = Stage::Attack;
    prevGate_ = 1.0;
}

double AdsrVoiceState::step (const ModEnvSpec& spec, double gate, double& playheadOut) noexcept
{
    const double att = isFiniteBitsD (spec.attack)  ? std::clamp (spec.attack,  0.001, 10.0) : 0.01;
    const double dec = isFiniteBitsD (spec.decay)   ? std::clamp (spec.decay,   0.001, 10.0) : 0.2;
    const double sus = isFiniteBitsD (spec.sustain) ? std::clamp (spec.sustain, 0.0,   1.0)  : 0.7;
    const double rel = isFiniteBitsD (spec.release) ? std::clamp (spec.release, 0.001, 10.0) : 0.3;

    if (gate > 0.0 && prevGate_ <= 0.0)
    {
        stage_ = Stage::Attack;
    }
    else if (gate <= 0.0 && prevGate_ > 0.0 && stage_ != Stage::Idle)
    {
        stage_ = Stage::Release;
        releaseStart_ = std::max (1.0e-6, env_);
    }
    prevGate_ = gate;

    switch (stage_)
    {
        case Stage::Idle:
            env_ = 0.0;
            playheadOut = 0.0;
            break;

        case Stage::Attack:
        {
            const double stepInc = 1.0 / std::max (1.0, att * sampleRate_);
            env_ += stepInc;
            if (env_ >= 1.0)
            {
                env_ = 1.0;
                stage_ = Stage::Decay;
            }
            playheadOut = std::clamp (0.25 * env_, 0.0, 0.25);
            break;
        }

        case Stage::Decay:
        {
            const double span = std::max (0.0, 1.0 - sus);
            const double stepDec = span / std::max (1.0, dec * sampleRate_);
            env_ -= stepDec;
            if (env_ <= sus || span <= 1.0e-9)
            {
                env_ = sus;
                stage_ = Stage::Sustain;
            }
            playheadOut = span > 1.0e-6
                              ? std::clamp (0.25 + 0.25 * ((1.0 - env_) / span), 0.25, 0.5)
                              : 0.5;
            break;
        }

        case Stage::Sustain:
            env_ = sus;
            playheadOut = 0.625;
            break;

        case Stage::Release:
        {
            const double start = std::max (1.0e-6, releaseStart_);
            const double stepRel = start / std::max (1.0, rel * sampleRate_);
            env_ -= stepRel;
            if (env_ <= 0.0)
            {
                env_ = 0.0;
                stage_ = Stage::Idle;
                playheadOut = 0.0;
            }
            else
            {
                playheadOut = std::clamp (0.75 + 0.25 * (1.0 - env_ / start), 0.75, 1.0);
            }
            break;
        }
    }

    smoothed_ += smoothCoeff_ * (env_ - smoothed_);
    if (! isFiniteBitsD (smoothed_))
        smoothed_ = 0.0;
    smoothed_ = std::clamp (smoothed_, 0.0, 1.0);
    return smoothed_;
}

double AdsrVoiceState::advance (const ModEnvSpec& spec, double gate, int64_t n, double& playheadOut) noexcept
{
    const double att = isFiniteBitsD (spec.attack)  ? std::clamp (spec.attack,  0.001, 10.0) : 0.01;
    const double dec = isFiniteBitsD (spec.decay)   ? std::clamp (spec.decay,   0.001, 10.0) : 0.2;
    const double sus = isFiniteBitsD (spec.sustain) ? std::clamp (spec.sustain, 0.0,   1.0)  : 0.7;
    const double rel = isFiniteBitsD (spec.release) ? std::clamp (spec.release, 0.001, 10.0) : 0.3;

    if (gate > 0.0 && prevGate_ <= 0.0)
    {
        stage_ = Stage::Attack;
    }
    else if (gate <= 0.0 && prevGate_ > 0.0 && stage_ != Stage::Idle)
    {
        stage_ = Stage::Release;
        releaseStart_ = std::max (1.0e-6, env_);
    }
    prevGate_ = gate;

    // Linear segments, so a jump of `left` samples is exact; at most
    // Attack -> Decay -> Sustain (or Release -> Idle) per call.
    double left = n > 0 ? static_cast<double> (n) : 0.0;
    for (int guard = 0; guard < 4 && left > 0.0; ++guard)
    {
        switch (stage_)
        {
            case Stage::Idle:
                env_ = 0.0;
                left = 0.0;
                break;

            case Stage::Attack:
            {
                const double inc = 1.0 / std::max (1.0, att * sampleRate_);
                const double need = (1.0 - env_) / inc;
                if (need <= left)
                {
                    env_ = 1.0;
                    stage_ = Stage::Decay;
                    left -= std::max (0.0, need);
                }
                else
                {
                    env_ += inc * left;
                    left = 0.0;
                }
                break;
            }

            case Stage::Decay:
            {
                const double span = std::max (0.0, 1.0 - sus);
                if (span <= 1.0e-9 || env_ <= sus)
                {
                    env_ = sus;
                    stage_ = Stage::Sustain;
                    break;
                }
                const double stepDec = span / std::max (1.0, dec * sampleRate_);
                const double need = (env_ - sus) / stepDec;
                if (need <= left)
                {
                    env_ = sus;
                    stage_ = Stage::Sustain;
                    left -= need;
                }
                else
                {
                    env_ -= stepDec * left;
                    left = 0.0;
                }
                break;
            }

            case Stage::Sustain:
                left = 0.0;
                break;

            case Stage::Release:
            {
                const double start = std::max (1.0e-6, releaseStart_);
                const double stepRel = start / std::max (1.0, rel * sampleRate_);
                if (env_ / stepRel <= left)
                {
                    env_ = 0.0;
                    stage_ = Stage::Idle;
                }
                else
                {
                    env_ -= stepRel * left;
                }
                left = 0.0;
                break;
            }
        }
    }
    if (stage_ == Stage::Sustain)
        env_ = sus;  // follows sustain edits
    if (! isFiniteBitsD (env_))
        env_ = 0.0;
    env_ = std::clamp (env_, 0.0, 1.0);

    switch (stage_)
    {
        case Stage::Idle:    playheadOut = 0.0; break;
        case Stage::Attack:  playheadOut = std::clamp (0.25 * env_, 0.0, 0.25); break;
        case Stage::Decay:
        {
            const double span = std::max (0.0, 1.0 - sus);
            playheadOut = span > 1.0e-6 ? std::clamp (0.25 + 0.25 * ((1.0 - env_) / span), 0.25, 0.5) : 0.5;
            break;
        }
        case Stage::Sustain: playheadOut = 0.625; break;
        case Stage::Release:
        {
            const double start = std::max (1.0e-6, releaseStart_);
            playheadOut = std::clamp (0.75 + 0.25 * (1.0 - env_ / start), 0.75, 1.0);
            break;
        }
    }
    return env_;
}

// ---------------------------------------------------------------------------
// VoiceModPack: the control-rate runner

double VoiceModPack::blockSmoothingCoeff (double sampleRate) noexcept
{
    // (1 - c)^N for the per-sample c = 1 - exp(-1 / (0.5 ms * rate)).
    const double sr = sampleRate > 1000.0 ? sampleRate : 48000.0;
    return 1.0 - std::exp (-static_cast<double> (kModControlBlock) / std::max (1.0, 0.0005 * sr));
}

void VoiceModPack::relive (uint8_t mask, const ModSpecs& sp, double bpm, double beat, double gate) noexcept
{
    // Newly ticking sources catch up to `age` and start from their raw
    // value (snapped, like an LFO's first sample); they join the shared
    // block clock at the next block.
    const uint8_t fresh = static_cast<uint8_t> (mask & ~live);
    for (int s = 0; s < kNumModSources; ++s)
    {
        if (((fresh >> s) & 1u) == 0u)
            continue;
        const int64_t n = std::max<int64_t> (0, age - doneTo[(size_t) s]);
        double raw = 0.0;
        if (s < kNumModLfos)
            raw = lfos[(size_t) s].advance (sp.lfos[(size_t) s], bpm, beat, n, phases[(size_t) s],
                                            lfoCycle[(size_t) s]);
        else
            raw = envs[(size_t) (s - kNumModLfos)].advance (sp.envs[(size_t) (s - kNumModLfos)], gate, n,
                                                            phases[(size_t) s]);
        target[(size_t) s] = raw;
        rawLast[(size_t) s] = raw;
        ramp[(size_t) s] = raw;
        values[(size_t) s] = raw;
        slope[(size_t) s] = 0.0;
        doneTo[(size_t) s] = age;
    }
    // Sources that stop ticking hold their value (tick() runs every source).
    const uint8_t gone = static_cast<uint8_t> (live & ~mask);
    for (int s = 0; s < kNumModSources; ++s)
        if (((gone >> s) & 1u) != 0u)
        {
            slope[(size_t) s] = 0.0;
            ramp[(size_t) s] = values[(size_t) s];
        }
    live = mask;
}

void VoiceModPack::beginBlock (uint8_t mask, const ModSpecs& sp, double bpm, double beatAtEnd, double gate) noexcept
{
    blockLeft = kModControlBlock - 1;  // this sample is the block's first
    constexpr double invBlock = 1.0 / kModControlBlock;
    for (int s = 0; s < kNumModSources; ++s)
    {
        if (((mask >> s) & 1u) == 0u)
            continue;
        const size_t i = (size_t) s;
        ramp[i] = target[i];  // exact block start (no ramp drift)
        double next = 0.0;
        bool hold = false;
        if (s < kNumModLfos)
        {
            int64_t cyc = 0;
            const ModLfoSpec& spec = sp.lfos[i];
            next = lfos[i].advance (spec, bpm, beatAtEnd, kModControlBlock, phases[i], cyc);
            hold = LfoVoiceState::holdsPerBlock (spec.shape)
                || (LfoVoiceState::jumpsAtWrap (spec.shape) && cyc != lfoCycle[i]);
            lfoCycle[i] = cyc;
        }
        else
        {
            next = envs[i - kNumModLfos].advance (sp.envs[i - kNumModLfos], gate, kModControlBlock, phases[i]);
        }
        // Held shapes (and a saw across its wrap) feed the smoother the
        // block-start value, so the jump enters at the next boundary.
        const double in = hold ? rawLast[i] : next;
        rawLast[i] = next;
        const double smoothed = target[i] + blockCoeff * (in - target[i]);
        slope[i] = (smoothed - target[i]) * invBlock;
        target[i] = smoothed;
    }
}

void VoiceModPack::catchUp (uint8_t mask, const ModSpecs& sp, double bpm, double beat, double gate) noexcept
{
    for (int s = 0; s < kNumModSources; ++s)
    {
        if (((mask >> s) & 1u) == 0u)
            continue;
        const size_t i = (size_t) s;
        const int64_t n = std::max<int64_t> (0, age - doneTo[i]);
        if (s < kNumModLfos)
        {
            int64_t cyc = 0;
            values[i] = lfos[i].advance (sp.lfos[i], bpm, beat, n, phases[i], cyc);
        }
        else
        {
            values[i] = envs[i - kNumModLfos].advance (sp.envs[i - kNumModLfos], gate, n, phases[i]);
        }
        ramp[i] = values[i];  // tick() holds it there
        slope[i] = 0.0;
        doneTo[i] = age;
    }
}

} // namespace vibesynth
