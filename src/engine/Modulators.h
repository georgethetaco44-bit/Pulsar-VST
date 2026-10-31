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
// Modulators run at control rate: one step per kModControlBlock voice
// samples. The step feeds the 0.5 ms one-pole (~2 ms to settle; the
// coefficient is the per-sample one compounded over the block) and the
// output ramps linearly between the smoothed points, so it moves every
// sample and never steps. S&H, square and a saw's wrap enter the smoother
// at the first block boundary after the jump (sharp, no look-ahead).
inline constexpr int kModControlBlock = 16;

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

    // Control rate: moves `n` voice samples on (retrigger) or to `beat`
    // (free-running) and returns the raw shape there (no smoothing); n = 0
    // reads the current point. phaseOut in [0, 1], cycleOut the cycle index.
    double advance (const ModLfoSpec& spec, double bpm, double beat, int64_t n, double& phaseOut,
                    int64_t& cycleOut) noexcept;

    // Pure waveform evaluation for UI preview and unit tests (phase in [0, 1),
    // cycleIndex used for deterministic S&H / smooth random).
    static double evalShape (LfoShape shape, double phase, int64_t cycleIndex, uint32_t seed) noexcept;

    // Shapes that hold per control block instead of ramping (S&H, square).
    static bool holdsPerBlock (LfoShape s) noexcept
    {
        return s == LfoShape::SampleAndHold || s == LfoShape::Square;
    }
    // Shapes with a jump at the cycle wrap (ramped only within a cycle).
    static bool jumpsAtWrap (LfoShape s) noexcept { return s == LfoShape::SawUp || s == LfoShape::SawDown; }

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

    // Control rate: handles a gate edge, moves `n` voice samples on (across
    // stage changes) and returns the raw level there (no smoothing); n = 0
    // reads the current level.
    double advance (const ModEnvSpec& spec, double gate, int64_t n, double& playheadOut) noexcept;

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

struct ModSpecs
{
    std::array<ModLfoSpec, kNumModLfos> lfos {};
    std::array<ModEnvSpec, kNumModEnvs> envs {};
};

// Macro modulation as a plan built once per render segment: the modulated
// macros and their depths per source (0 for unassigned sources, so the
// per-sample sum needs no mask).
struct MacroModPlan
{
    int count = 0;
    int index[8] = {};
    double depth[8][kNumModSources] = {};

    // out[0..8) = in with the planned macros modulated by `mod` and clamped.
    void apply (const double* in, const double* mod, double* out) const noexcept
    {
        for (int m = 0; m < 8; ++m)
            out[m] = in[m];
        for (int k = 0; k < count; ++k)
        {
            const double* d = depth[k];
            const double v = in[index[k]] + d[0] * mod[0] + d[1] * mod[1] + d[2] * mod[2] + d[3] * mod[3];
            out[index[k]] = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        }
    }
};

struct VoiceModPack
{
    std::array<LfoVoiceState, kNumModLfos> lfos;
    std::array<AdsrVoiceState, kNumModEnvs> envs;
    std::array<double, kNumModSources> values {};  // smoothed (ticking) or raw (display) output
    std::array<double, kNumModSources> phases {};

    // Control-rate runner state (tick()).
    std::array<double, kNumModSources> ramp {};    // linear ramp, current sample
    std::array<double, kNumModSources> slope {};   // per voice sample
    std::array<double, kNumModSources> target {};  // the ramp's end: the smoothed control point
    std::array<double, kNumModSources> rawLast {}; // raw shape at the current block's start
    std::array<int64_t, kNumModLfos> lfoCycle {};
    std::array<int64_t, kNumModSources> doneTo {}; // age each source has been advanced to
    int64_t age = 0;                               // voice samples since note-on (per segment)
    double blockCoeff = 1.0;                       // 0.5 ms one-pole over kModControlBlock samples
    int blockLeft = 0;
    uint8_t live = 0;                              // sources ticking with a valid ramp

    void prepare (double sampleRate, int voiceIndex) noexcept
    {
        for (int i = 0; i < kNumModLfos; ++i)
            lfos[(size_t) i].prepare (sampleRate, i, voiceIndex);
        for (int i = 0; i < kNumModEnvs; ++i)
            envs[(size_t) i].prepare (sampleRate);
        blockCoeff = blockSmoothingCoeff (sampleRate);
        clearRunner();
        values.fill (0.0);
        phases.fill (0.0);
    }

    void reset() noexcept
    {
        for (auto& l : lfos)
            l.reset();
        for (auto& e : envs)
            e.reset();
        clearRunner();
        values.fill (0.0);
        phases.fill (0.0);
    }

    void noteOn (int midiNote) noexcept
    {
        for (auto& l : lfos)
            l.noteOn (midiNote);
        for (auto& e : envs)
            e.noteOn();
        clearRunner();
    }

    // Per-sample reference stepping (unit tests): every source, every sample.
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

    // Audio thread, once per voice sample, for the sources in `mask` (the
    // ones with an assignment): a control-rate step every kModControlBlock
    // samples, then one add per source. `beat` is this sample's beat,
    // `beatStep` beats per voice sample. Other sources are left alone.
    void tick (uint8_t mask, const ModSpecs& sp, double bpm, double beat, double beatStep, double gate) noexcept
    {
        if (mask != live)
            relive (mask, sp, bpm, beat, gate);
        if (--blockLeft < 0)
            beginBlock (mask, sp, bpm, beat + kModControlBlock * beatStep, gate);
        // Every source: one that isn't ticking has slope 0 and ramp == its
        // value (relive/catchUp keep that), so no mask test is needed.
        for (int s = 0; s < kNumModSources; ++s)
        {
            values[(size_t) s] = ramp[(size_t) s];
            ramp[(size_t) s] += slope[(size_t) s];
        }
    }

    // After each render segment: the voice rendered `n` more samples; the
    // ticking sources are current.
    void endSegment (int64_t n) noexcept
    {
        age += n;
        for (int s = 0; s < kNumModSources; ++s)
            if ((live >> s) & 1u)
                doneTo[(size_t) s] = age;
    }

    // Display only (the voice the MOD tiles show): brings the sources in
    // `mask` up to `age` in one jump; values are the raw shapes (no ramp,
    // no smoothing). `beat` is the beat at the end of the segment.
    void catchUp (uint8_t mask, const ModSpecs& sp, double bpm, double beat, double gate) noexcept;

    // The 0.5 ms one-pole's coefficient compounded over one control block.
    static double blockSmoothingCoeff (double sampleRate) noexcept;

private:
    void clearRunner() noexcept
    {
        ramp.fill (0.0);
        slope.fill (0.0);
        target.fill (0.0);
        rawLast.fill (0.0);
        lfoCycle.fill (0);
        doneTo.fill (0);
        age = 0;
        blockLeft = 0;
        live = 0;
    }

    void relive (uint8_t mask, const ModSpecs& sp, double bpm, double beat, double gate) noexcept;
    void beginBlock (uint8_t mask, const ModSpecs& sp, double bpm, double beatAtEnd, double gate) noexcept;
};

} // namespace vibesynth
