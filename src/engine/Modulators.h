#pragma once

// M13 (SPEC §9.8): 2 LFOs + 2 ADSR envelopes per voice, plus modulation
// routing data structures shared by Patch, OverrideTable and PatchPlayer.
//
// JUCE-free and real-time safe: no allocations, locks, or std::function on
// the audio thread.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vibesynth {

inline constexpr int kNumModLfos = 2;
inline constexpr int kNumModEnvs = 2;
inline constexpr int kNumModSources = kNumModLfos + kNumModEnvs; // LFO 1, LFO 2, Env 1, Env 2
inline constexpr int kMaxModAssignments = 8;
inline constexpr double kHzModSemitoneRange = 24.0; // matches M10's ±24 st Offset range

enum class ModSource : int
{
    Lfo1 = 0,
    Lfo2 = 1,
    Env1 = 2,
    Env2 = 3
};

enum class LfoShape : int
{
    Sine = 0,
    Triangle,
    SawUp,
    SawDown,
    Square,
    SampleAndHold,
    SmoothRandom,
    Count
};

// Host-synced beat divisions from 1/32 to 4 bars, including triplet (T) and
// dotted (D) variants.
enum class LfoSyncDiv : int
{
    Div1_32T = 0,
    Div1_32,
    Div1_32D,
    Div1_16T,
    Div1_16,
    Div1_16D,
    Div1_8T,
    Div1_8,
    Div1_8D,
    Div1_4T,
    Div1_4,
    Div1_4D,
    Div1_2T,
    Div1_2,
    Div1_2D,
    Div1_1T,
    Div1_1,
    Div1_1D,
    Div2_1T,
    Div2_1,
    Div2_1D,
    Div4_1T,
    Div4_1,
    Div4_1D,
    Count
};

const char* modSourceId (ModSource s) noexcept;
const char* modSourceLabel (ModSource s) noexcept;
bool parseModSourceId (const std::string& s, ModSource& out) noexcept;

const char* lfoShapeId (LfoShape s) noexcept;
const char* lfoShapeLabel (LfoShape s) noexcept;
bool parseLfoShapeId (const std::string& s, LfoShape& out) noexcept;

const char* lfoSyncDivId (LfoSyncDiv d) noexcept;
const char* lfoSyncDivLabel (LfoSyncDiv d) noexcept;
double lfoSyncDivBeats (LfoSyncDiv d) noexcept; // period in quarter-note beats
bool parseLfoSyncDivId (const std::string& s, LfoSyncDiv& out) noexcept;

struct ModLfoSpec
{
    LfoShape shape = LfoShape::Sine;
    bool sync = true;
    double rateHz = 2.0;                   // 0.05 .. 30.0 Hz when !sync
    LfoSyncDiv syncDiv = LfoSyncDiv::Div1_4;
    bool retrigger = false;                // true = per-note retrigger; false = free (host beat)

    bool isDefault() const noexcept;
    bool operator== (const ModLfoSpec& o) const noexcept;
    bool operator!= (const ModLfoSpec& o) const noexcept { return ! (*this == o); }
};

struct ModEnvSpec
{
    double attack = 0.01;   // seconds (0.001 .. 10.0)
    double decay = 0.2;     // seconds (0.001 .. 10.0)
    double sustain = 0.7;   // level   (0.0 .. 1.0)
    double release = 0.3;   // seconds (0.001 .. 10.0)

    bool isDefault() const noexcept;
    bool operator== (const ModEnvSpec& o) const noexcept;
    bool operator!= (const ModEnvSpec& o) const noexcept { return ! (*this == o); }
};

// A modulation assignment targets either a macro (macro = 1..8, node empty)
// or a node argument (macro = 0, node = "flt_lp:0", arg = "cutoff").
struct ModAssignment
{
    ModSource source = ModSource::Lfo1;
    int macro = 0;          // 1..8 for macro target; 0 for node-knob target
    std::string node;       // node id when macro == 0
    std::string arg;        // arg name when macro == 0
    double depth = 0.0;     // bipolar -1.0 .. +1.0

    bool isMacroTarget() const noexcept { return macro >= 1 && macro <= 8 && node.empty(); }
    bool isNodeTarget() const noexcept { return macro == 0 && ! node.empty() && ! arg.empty(); }
    bool valid() const noexcept { return isMacroTarget() || isNodeTarget(); }

    bool sameTarget (const ModAssignment& o) const noexcept
    {
        return macro == o.macro && node == o.node && arg == o.arg;
    }

    bool operator== (const ModAssignment& o) const noexcept;
    bool operator!= (const ModAssignment& o) const noexcept { return ! (*this == o); }
};

struct PatchMods
{
    std::array<ModLfoSpec, kNumModLfos> lfos {};
    std::array<ModEnvSpec, kNumModEnvs> envs {};
    std::vector<ModAssignment> assignments; // at most kMaxModAssignments

    bool empty() const noexcept
    {
        if (! assignments.empty())
            return false;
        for (const auto& l : lfos)
            if (! l.isDefault())
                return false;
        for (const auto& e : envs)
            if (! e.isDefault())
                return false;
        return true;
    }

    bool operator== (const PatchMods& o) const noexcept
    {
        return lfos == o.lfos && envs == o.envs && assignments == o.assignments;
    }
    bool operator!= (const PatchMods& o) const noexcept { return ! (*this == o); }
};

// ---------------------------------------------------------------------------
// Per-voice DSP state for LFOs and ADSR envelopes.

class LfoVoiceState
{
public:
    void prepare (double sampleRate, int lfoIndex, int voiceIndex) noexcept;
    void reset() noexcept;
    void noteOn (int midiNote) noexcept;

    // Evaluates one sample in [-1, +1] and updates phaseOut in [0, 1).
    double step (const ModLfoSpec& spec, double bpm, double beat, double& phaseOut) noexcept;

    // Pure waveform evaluation for UI preview and unit tests (phase in [0, 1),
    // cycleIndex used for deterministic S&H / smooth random).
    static double evalShape (LfoShape shape, double phase, int64_t cycleIndex, uint32_t seed) noexcept;

private:
    double sampleRate_ = 48000.0;
    double invSampleRate_ = 1.0 / 48000.0;
    double smoothCoeff_ = 1.0;
    double phase_ = 0.0;
    int64_t cycleCount_ = 0;
    double smoothed_ = 0.0;
    bool firstSample_ = true;
    uint32_t baseSeed_ = 1u;
    uint32_t noteSeed_ = 1u;
};

class AdsrVoiceState
{
public:
    enum class Stage : int
    {
        Idle = 0,
        Attack,
        Decay,
        Sustain,
        Release
    };

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void noteOn() noexcept;

    // Evaluates one sample in [0, 1] and updates playheadOut in [0, 1]
    // (0..0.25 Attack, 0.25..0.5 Decay, 0.5..0.75 Sustain, 0.75..1.0 Release).
    double step (const ModEnvSpec& spec, double gate, double& playheadOut) noexcept;

    Stage stage() const noexcept { return stage_; }
    double level() const noexcept { return smoothed_; }

private:
    double sampleRate_ = 48000.0;
    double smoothCoeff_ = 1.0;
    Stage stage_ = Stage::Idle;
    double env_ = 0.0;
    double smoothed_ = 0.0;
    double releaseStart_ = 0.0;
    double prevGate_ = 0.0;
};

struct VoiceModPack
{
    std::array<LfoVoiceState, kNumModLfos> lfos;
    std::array<AdsrVoiceState, kNumModEnvs> envs;
    std::array<double, kNumModSources> values {};
    std::array<double, kNumModSources> phases {};

    void prepare (double sampleRate, int voiceIndex) noexcept
    {
        for (int i = 0; i < kNumModLfos; ++i)
            lfos[(size_t) i].prepare (sampleRate, i, voiceIndex);
        for (int i = 0; i < kNumModEnvs; ++i)
            envs[(size_t) i].prepare (sampleRate);
        values.fill (0.0);
        phases.fill (0.0);
    }

    void reset() noexcept
    {
        for (auto& l : lfos)
            l.reset();
        for (auto& e : envs)
            e.reset();
        values.fill (0.0);
        phases.fill (0.0);
    }

    void noteOn (int midiNote) noexcept
    {
        for (auto& l : lfos)
            l.noteOn (midiNote);
        for (auto& e : envs)
            e.noteOn();
    }

    void stepAll (const std::array<ModLfoSpec, kNumModLfos>& lfoSpecs,
                  const std::array<ModEnvSpec, kNumModEnvs>& envSpecs,
                  double bpm,
                  double beat,
                  double gate) noexcept
    {
        for (int i = 0; i < kNumModLfos; ++i)
            values[(size_t) i] = lfos[(size_t) i].step (lfoSpecs[(size_t) i], bpm, beat, phases[(size_t) i]);
        for (int i = 0; i < kNumModEnvs; ++i)
            values[(size_t) (kNumModLfos + i)] = envs[(size_t) i].step (envSpecs[(size_t) i], gate,
                                                                        phases[(size_t) (kNumModLfos + i)]);
    }
};

} // namespace vibesynth
