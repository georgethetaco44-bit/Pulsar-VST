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

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "engine/OutputStage.h"
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

    PatchPlayer();

    // Off the audio thread: sizes the chunk buffers, resets the voices,
    // guard, output stage and smoother (snapped to `macroTargets`).
    void prepare (double sampleRate, int maxChunk, const float macroTargets[kNumMacros]);

    // The program being played (not owned). setProgram() swaps it with no
    // voice handling (prepare/teardown only).
    PatchProgram* program() const noexcept { return program_; }
    void setProgram (PatchProgram* p) noexcept { program_ = p; }

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
            if (at > pos && program_ != nullptr)
            {
                renderVoices (pos, at);
                pos = at;
            }
            eventTransport_.beat = chunkBeat_ + at * beatStep_;
            handleMidi (event.data, event.numBytes);
        }
        if (pos < n && program_ != nullptr)
            renderVoices (pos, n);
        finishChunk (n);
    }

    const float* outL() const noexcept { return outL_.data(); }
    const float* outR() const noexcept { return outR_.data(); }

    // Raw MIDI (note on/off, CC 120 all sound off, CC 123 all notes off).
    // A note-on's init code sees bpm/beat at the event's sample.
    void handleMidi (const uint8_t* data, int numBytes) noexcept;

    static double noteHz (int note) noexcept;

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
        double freq = 0.0;
        double vel = 0.0;
        uint64_t startOrder = 0;      // note-on order: smaller = older
        bool renderedSinceNoteOn = false;
        bool releasePending = false;  // note-off before the note-on sample
        bool hasPending = false;
        int pendingNote = -1;
        double pendingVel = 0.0;
        bool pendingReleased = false;
        int muteSamples = 0;          // runtime guard: silent, not rendered
    };

    EelVoice& voiceOf (const VoiceSlot& v) noexcept { return program_->voice (v.index); }
    void clearSlot (VoiceSlot& v) noexcept;
    void startNote (int note, double velocity) noexcept;
    void stopNote (int note) noexcept;
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

    void beginChunk (int n, const float targets[kNumMacros]) noexcept;
    void renderVoices (int start, int end) noexcept;
    void finishChunk (int n) noexcept;

    std::array<VoiceSlot, kNumVoiceObjects> voices_;
    PatchProgram* program_ = nullptr;
    MacroSmoother smoother_;
    OutputStage outputStage_;
    std::vector<float> voiceSum_;     // mono voice mix for one chunk
    std::vector<double> macroBlock_;  // smoothed macros, kNumMacros per sample
    std::vector<float> outL_;
    std::vector<float> outR_;
    int chunkCapacity_ = 0;
    int guardMuteLength_ = 960;
    int masterMuteSamples_ = 0;
    double sampleRate_ = 48000.0;
    uint64_t noteCounter_ = 0;
    Transport transport_;          // tempo, and the beat at the next chunk's start
    Transport eventTransport_;     // bpm/beat for note-ons (MIDI event sample)
    double chunkBeat_ = 0.0;       // beat at sample 0 of the current chunk
    double beatStep_ = 0.0;        // beats per sample
};

} // namespace vibesynth
