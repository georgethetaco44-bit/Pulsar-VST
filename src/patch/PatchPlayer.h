#pragma once

// PatchPlayer (M6): plays a PatchProgram. Voice slots and stealing
// (SPEC §9.1), sample-accurate MIDI, the macro smoother, the master VM,
// the runtime guard (§7.2) and the output stage (trim -> DC block ->
// limiter -> clip). Moved out of VibeSynthAudioProcessor so that
// vibesynth_cli --render runs exactly the plugin's chain (§10.3); the
// processor keeps the threads, queues and the swap fade around it.
//
// JUCE-free, part of vibesynth_engine. prepare() allocates and runs off
// the audio thread; everything else is audio-thread safe (SPEC §11): no
// allocation, locks, logging, I/O or exceptions. The player never owns or
// frees a program.
//
// Voice stealing (SPEC 9.1, amended): at most 8 voices SOUND at once
// (Active, or Fading with a queued note). A 9th note-on gives the steal
// victim (the oldest released voice, else the oldest held one) a 2 ms
// linear fade and starts the new note at once on a free spare. Only when no
// spare is free is a voice hard-stolen: it fades for 2 ms in place, then its
// rack is reset and the queued note starts. A voice is freed by
// VoiceLifetime (-80 dB for 50 ms after gate-off, 10 s cap) or when its
// steal fade ends.
//
// M7 (SPEC §4.2, §9.1):
//   - 2x oversampling: the voices run at the program's voice rate
//     (oversampling x host), MIDI stays sample-accurate at the host rate
//     (an event at host sample n lands before voice sample n x os), and
//     the mono voice sum goes through one HalfbandDecimator before the
//     master, which runs at the host rate. The steal fade, VoiceLifetime
//     and the guard's voice mute count voice samples; macros, mw and pitch
//     bend are smoothed per host sample and held across its voice samples.
//   - Voice mode (Patch / Poly / Mono, "Patch" follows the patch's
//     voiceMode) and glide, set per host block with setVoiceControls().
//     Mono: last-note priority with a held-note stack. A note-on while a
//     key is held glides (legato: no trig, no init, gate stays 1);
//     releasing the top note glides back to the newest one still held; a
//     note-on after all keys are up retriggers the same voice in place
//     (trig, init, no steal fade, no glide). Glide time = glide^2 s (0–1 s),
//     linear in pitch (exponential in Hz). A change of the effective mode
//     releases every held note.
//   - Pitch bend ±2 semitones, smoothed (~5 ms), applied to `freq` after
//     glide; CC1 is `mw` (0–1, smoothed like the macros) in every VM.

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "engine/NodeMeters.h"
#include "engine/NodeOverrides.h"
#include "engine/OutputStage.h"
#include "engine/Oversampler.h"
#include "patch/PatchEngine.h"
#include "patch/PatchValidator.h"

namespace vibesynth {

class PatchPlayer
{
public:
    static constexpr int kNumMacros = MacroSmoother::kNumMacros;
    static constexpr int kMaxSoundingVoices = 8;                  // SPEC 9.1 polyphony
    static constexpr int kNumVoiceObjects = PatchProgram::kNumVoices; // + 2 spares
    static constexpr double kGuardMuteSeconds = 0.020;            // SPEC 7.2
    static constexpr double kPitchBendRange = 2.0;                // semitones, SPEC 9.1
    static constexpr double kPitchBendSeconds = 0.005;            // smoothing
    static constexpr double kMaxGlideSeconds = 1.0;

    // The "Voice Mode" host parameter's choices, in parameter order.
    enum class VoiceMode { Patch = 0, Poly = 1, Mono = 2 };

    PatchPlayer();

    // Off the audio thread: sizes the chunk buffers, resets the voices,
    // guard, output stage, decimator, held-note stack, pitch bend, mod
    // wheel and the macro smoother (snapped to `macroTargets`).
    // `sampleRate` is the host rate.
    void prepare (double sampleRate, int maxChunk, const float macroTargets[kNumMacros]);

    // The program being played (not owned). setProgram() swaps it with no
    // voice handling (prepare/teardown only).
    PatchProgram* program() const noexcept { return program_; }
    void setProgram (PatchProgram* p) noexcept
    {
        program_ = p;
        configureHooks (false);
    }

    // M10 (SPEC §9.6): the processor's override table and trace bank (not
    // owned; null = none). Off the audio thread, before rendering. Each
    // chunk the player points every rack of the current program at the
    // table (`hooked` only while the table has something for that rack),
    // and the newest sounding voice and the master at the traces while
    // traces->enabled.
    void setNodeHooks (const OverrideTable* table, TraceBank* traces) noexcept
    {
        overrides_ = table;
        traces_ = traces;
        configureHooks (false);
    }

    // Audio thread (SPEC 7.3): plays `next` from now on and returns the old
    // program. Held notes restart on their new voices (trig = 1, init
    // runs); released and fading voices are dropped, except a queued
    // hard-steal note, which starts now.
    PatchProgram* swapProgram (PatchProgram* next) noexcept;

    int maxChunk() const noexcept { return chunkCapacity_; }

    // Host tempo (SPEC §4.3), once per host block before renderChunk.
    // `bpm` <= 0 or non-finite means the host gives none (120). While the
    // host plays and gives a PPQ position, `beat` jumps to it; otherwise it
    // runs on from where it was (prepare() starts it at 0).
    void setHostTransport (double bpm, bool playing, bool hasPpq, double ppq) noexcept;
    const Transport& transport() const noexcept { return transport_; }

    // Voice mode and glide (0–1), once per host block before renderChunk.
    // The effective mode is applied at the next chunk.
    void setVoiceControls (VoiceMode mode, double glide) noexcept;
    bool monoActive() const noexcept { return monoActive_; }

    // Renders n <= maxChunk() samples into outL()/outR() (after the output
    // stage). Macros are smoothed towards `targets` per sample. MIDI events
    // come from [it, end): any iterator whose items have .samplePosition,
    // .data and .numBytes (JUCE's MidiBuffer iterator, or the CLI's own
    // event list). Positions are relative to `offset`; events at or past
    // offset + n stay unconsumed in `it`.
    template <typename Iter>
    void renderChunk (int n, const float targets[kNumMacros], Iter& it, const Iter& end,
                      int offset) noexcept
    {
        beginChunk (n, targets);
        int pos = 0;
        for (; it != end; ++it)
        {
            const auto event = *it;
            const int at = std::max (event.samplePosition - offset, pos);
            if (at >= n)
                break;
            if (at > pos)
            {
                fillControls (pos, at);
                if (program_ != nullptr)
                    renderVoices (pos, at);
                pos = at;
            }
            eventTransport_.beat = chunkBeat_ + at * beatStep_;
            eventMw_ = mwCurrent_;
            eventBend_ = bendRatio_;
            handleMidi (event.data, event.numBytes);
        }
        if (pos < n)
        {
            fillControls (pos, n);
            if (program_ != nullptr)
                renderVoices (pos, n);
        }
        finishChunk (n);
    }

    // M9 (SPEC §9.6, §11): once per host block, moves every voice's and
    // the master's rack meter peaks into `bank` (the max over sounding
    // voices; a free voice's leftovers are cleared, not published) and
    // zeroes them in the racks. Audio thread; no allocation or locks.
    void harvestMeters (NodeMeterBank& bank) noexcept;

    const float* outL() const noexcept { return outL_.data(); }
    const float* outR() const noexcept { return outR_.data(); }

    // Raw MIDI: note on/off, pitch bend, CC 1 (mod wheel), CC 120 (all
    // sound off), CC 123 (all notes off). A note-on's init code sees
    // bpm/beat, mw and the bent freq at the event's sample.
    void handleMidi (const uint8_t* data, int numBytes) noexcept;

    static double noteHz (int note) noexcept;

    // Tests (off the audio thread, between chunks): the slot's voice
    // object, its note and its glided frequency before pitch bend, or -1 /
    // 0 when the slot is free. Slots are 0..kNumVoiceObjects-1.
    int slotVoiceIndex (int slot) const noexcept;
    int slotNote (int slot) const noexcept;
    double slotFreq (int slot) const noexcept;
    bool slotGate (int slot) const noexcept;
    int soundingVoices() const noexcept;
    double pitchBendRatio() const noexcept { return bendRatio_; }

private:
    // Active = playing (gate held or releasing); Fading = stolen, in its
    // 2 ms fade, optionally with a note queued to start in place when the
    // fade ends (hard steal).
    struct VoiceSlot
    {
        enum class State { Free, Active, Fading };

        int index = 0;                // voice object in the current program
        State state = State::Free;
        VoiceLifetime lifetime;
        int note = -1;
        double freq = 0.0;            // glided, before pitch bend
        double vel = 0.0;
        double glideTarget = 0.0;     // Hz, while glideLeft > 0
        double glideRatio = 1.0;      // per voice sample
        int glideLeft = 0;            // voice samples
        uint64_t startOrder = 0;      // note-on order: smaller = older
        bool renderedSinceNoteOn = false;
        bool releasePending = false;  // note-off before the note-on sample
        bool hasPending = false;
        int pendingNote = -1;
        double pendingVel = 0.0;
        bool pendingReleased = false;
        int muteSamples = 0;          // runtime guard: silent, not rendered (voice samples)
    };

    EelVoice& voiceOf (const VoiceSlot& v) noexcept { return program_->voice (v.index); }
    double voiceRate() const noexcept { return program_ != nullptr ? program_->voiceRate() : sampleRate_; }
    int oversampling() const noexcept { return program_ != nullptr ? program_->oversampling() : 1; }
    void clearSlot (VoiceSlot& v) noexcept;
    VoiceSlot* startNote (int note, double velocity) noexcept;
    void stopNote (int note) noexcept;
    void releaseSlot (VoiceSlot& v) noexcept;
    void allNotesOff() noexcept;
    void allSoundOff() noexcept;
    void beginNote (VoiceSlot& v, int note, double velocity) noexcept;
    void beginFade (VoiceSlot& v) noexcept;
    VoiceSlot* findFree() noexcept;
    // Oldest slot (smallest startOrder) matching a captureless predicate.
    VoiceSlot* oldestWhere (bool (*pred) (const VoiceSlot&)) noexcept;
    // Steal victim among Active voices: the oldest released one, else the
    // oldest held one.
    VoiceSlot* stealVictim() noexcept;
    void tripVoice (VoiceSlot& v) noexcept;

    // Mono (M7).
    void monoNoteOn (int note, double velocity) noexcept;
    void monoNoteOff (int note) noexcept;
    VoiceSlot* monoSlot() noexcept;
    void glideTo (VoiceSlot& v, int note) noexcept;
    void pushHeld (int note) noexcept;
    bool removeHeld (int note) noexcept;
    void clearHeld() noexcept { heldCount_ = 0; }

    void beginChunk (int n, const float targets[kNumMacros]) noexcept;
    // M10: rack hook pointers for the current program (see setNodeHooks).
    void configureHooks (bool newChunk) noexcept;
    // Smoothed mod wheel and pitch-bend ratio for host samples [from, to).
    void fillControls (int from, int to) noexcept;
    void renderVoices (int start, int end) noexcept;
    void finishChunk (int n) noexcept;

    std::array<VoiceSlot, kNumVoiceObjects> voices_;
    PatchProgram* program_ = nullptr;
    MacroSmoother smoother_;
    OutputStage outputStage_;
    HalfbandDecimator decimator_;
    std::vector<float> voiceSum_;     // mono voice mix for one chunk (voice rate)
    std::vector<double> macroBlock_;  // smoothed macros, kNumMacros per host sample
    std::vector<double> mwBlock_;     // smoothed mod wheel per host sample
    std::vector<double> bendBlock_;   // pitch-bend frequency ratio per host sample
    std::vector<float> outL_;
    std::vector<float> outR_;
    int chunkCapacity_ = 0;
    int masterMuteLength_ = 960;
    int masterMuteSamples_ = 0;
    double sampleRate_ = 48000.0;     // host rate
    uint64_t noteCounter_ = 0;
    Transport transport_;          // tempo, and the beat at the next chunk's start
    Transport eventTransport_;     // bpm/beat for note-ons (MIDI event sample)
    double chunkBeat_ = 0.0;       // beat at sample 0 of the current chunk
    double beatStep_ = 0.0;        // beats per host sample

    // Voice mode, glide, held-note stack (mono).
    VoiceMode modeSetting_ = VoiceMode::Patch;
    double glide_ = 0.0;
    bool monoActive_ = false;
    int monoIndex_ = -1;           // slot of the mono voice (-1: none)
    std::array<uint8_t, 128> held_ {};
    int heldCount_ = 0;

    // Mod wheel and pitch bend (per host sample).
    double mwTarget_ = 0.0, mwCurrent_ = 0.0, mwCoeff_ = 1.0;
    double bendTarget_ = 0.0, bendCurrent_ = 0.0, bendCoeff_ = 1.0;
    double bendRatio_ = 1.0;
    double eventMw_ = 0.0;
    double eventBend_ = 1.0;

    // M10 hooks (not owned).
    const OverrideTable* overrides_ = nullptr;
    TraceBank* traces_ = nullptr;
};

} // namespace vibesynth
