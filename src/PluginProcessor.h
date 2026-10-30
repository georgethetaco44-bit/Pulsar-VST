#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine/OutputStage.h"
#include "patch/Patch.h"
#include "patch/PatchEngine.h"
#include "patch/PatchValidator.h"

//==============================================================================
/**
    VibeSynthAudioProcessor, Milestone 5 (SPEC section 13, M5).

    - 8 automatable float parameters "Macro 1".."Macro 8" (0..1, default 0.5)
      exposed via AudioProcessorValueTreeState, smoothed per sample by
      vibesynth::MacroSmoother and bound to m1..m8 in every VM.
    - The sound is a vibesynth::PatchProgram (10 EEL2 voices + 1 master,
      compiled from a .pulsar patch). A worker thread parses, compiles and
      validates patches and hands programs to the audio thread through a
      lock-free SPSC swap queue; retired programs come back on a second
      SPSC queue and the worker frees them (SPEC 3, 7.3). A swap fades out
      over 5 ms, swaps, and fades in over 5 ms; held notes restart on their
      new voices.
    - Voice slot state (note, gate, steal fade) lives here, so it survives
      a swap. At most 8 voices sound at once; the 2 spares let a stolen
      voice fade out over 2 ms while the new note starts immediately.
    - Runtime guard (SPEC 7.2): a voice that outputs a non-finite sample is
      zeroed, reset (rack + VM variables + mem[]) and muted for 20 ms; the
      master gets the same treatment. Output chain: trim -> DC block ->
      limiter (-0.3 dBFS) -> clip -> final non-finite check.
    - State (SPEC 9.3): the APVTS tree plus the patch JSON as a property.
      Restoring recompiles the saved code (no offline matrix).
    - The built-in presets are the host programs.
*/
class VibeSynthAudioProcessor final : public juce::AudioProcessor,
                                      public juce::ChangeBroadcaster,
                                      private juce::AsyncUpdater
{
public:
    //==============================================================================
    VibeSynthAudioProcessor();
    ~VibeSynthAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Patch loading (message thread; the editor and the host program API).
    // Loads run the full compile + offline test on the worker; a rejected
    // patch leaves the current sound playing and the reason in the status.
    void loadBuiltin (int index);
    void loadPatchFile (const juce::File& file);
    // The active patch as .pulsar text (for Save...).
    std::string getActivePatchText() const;
    juce::String getActivePatchName() const;
    // Message thread only (updated before sendChangeMessage()).
    juce::String getStatusText() const { return statusText; }
    juce::String getMacroLabel (int index) const;
    // Message thread: sets the status line and notifies the editor.
    void setStatus (const juce::String& s);

    //==============================================================================
    static constexpr int numMacros = vibesynth::MacroSmoother::kNumMacros;
    static constexpr int maxSoundingVoices = 8;  // SPEC 9.1 polyphony
    static constexpr int numVoiceObjects = vibesynth::PatchProgram::kNumVoices; // + 2 spares
    static constexpr double swapFadeSeconds = 0.005;   // SPEC 7.3
    static constexpr double guardMuteSeconds = 0.020;  // SPEC 7.2

    juce::AudioProcessorValueTreeState apvts;

private:
    using PatchProgram = vibesynth::PatchProgram;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    //==============================================================================
    // Lock-free single-producer/single-consumer queue of owned programs.
    template <int Capacity>
    struct ProgramQueue
    {
        bool push (PatchProgram* p) noexcept
        {
            const auto scope = fifo.write (1);
            if (scope.blockSize1 > 0) slots[(size_t) scope.startIndex1] = p;
            else if (scope.blockSize2 > 0) slots[(size_t) scope.startIndex2] = p;
            else return false;
            return true;
        }

        PatchProgram* pop() noexcept
        {
            const auto scope = fifo.read (1);
            if (scope.blockSize1 > 0) return slots[(size_t) scope.startIndex1];
            if (scope.blockSize2 > 0) return slots[(size_t) scope.startIndex2];
            return nullptr;
        }

        juce::AbstractFifo fifo { Capacity };
        std::array<PatchProgram*, (size_t) Capacity> slots {};
    };

    //==============================================================================
    // Worker thread: parses, compiles and validates patch jobs, pushes the
    // finished programs to the audio thread and frees retired ones.
    struct LoadJob
    {
        enum class Kind { Load, Restore };
        Kind kind = Kind::Load;
        std::string text;
        juce::String source;     // shown in the status line
        uint64_t generation = 0;
    };

    struct LoadResult
    {
        bool ok = false;
        juce::String message;
        vibesynth::Patch patch;  // labels/defaults for the UI when ok
        bool setMacros = false;  // a Load (not a restore) sets macros to the patch defaults
        uint64_t generation = 0;
    };

    class Worker final : public juce::Thread
    {
    public:
        explicit Worker (VibeSynthAudioProcessor& p) : juce::Thread ("Pulsar patch worker"), owner (p) {}
        void run() override;

    private:
        VibeSynthAudioProcessor& owner;
    };

    void postJob (LoadJob job);
    bool takeJob (LoadJob& job);
    void processJob (const LoadJob& job);
    // Builds `patch` for the prepared rate and pushes it (worker, holding
    // buildMutex). Returns false with `error` set if it doesn't compile.
    bool buildAndPushLocked (const vibesynth::Patch& patch, std::string& error);
    void freeRetired() noexcept;  // caller holds buildMutex (single consumer)
    void postResult (LoadResult result);
    void handleAsyncUpdate() override;

    //==============================================================================
    // Voice slots (audio thread). Active = playing (gate held or
    // releasing); Fading = stolen, in its 2 ms fade, optionally with a note
    // queued to start in place when the fade ends (hard steal).
    struct VoiceSlot
    {
        enum class State { Free, Active, Fading };

        int index = 0;                // voice object in the current program
        State state = State::Free;
        vibesynth::VoiceLifetime lifetime;
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

    vibesynth::EelVoice& voiceOf (const VoiceSlot& v) noexcept { return program->voice (v.index); }
    void handleMidi (const juce::uint8* data, int numBytes) noexcept;
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
    void renderVoices (int start, int end) noexcept;
    void tripVoice (VoiceSlot& v) noexcept;

    // Swap protocol (audio thread).
    void pollSwapQueue() noexcept;
    void swapPrograms() noexcept;
    void retire (PatchProgram* p) noexcept;
    void flushRetireStash() noexcept;
    int samplesUntilSilent() const noexcept;

    void resetVoiceSlots() noexcept;

    //==============================================================================
    // Audio-thread state. prepareToPlay and the destructor also touch it,
    // which is safe because the host never runs them concurrently with
    // processBlock.
    std::array<VoiceSlot, numVoiceObjects> voices;
    PatchProgram* program = nullptr;   // current sound
    PatchProgram* incoming = nullptr;  // received, waiting for the fade-out
    enum class Fade { None, Out, In };
    Fade fade = Fade::None;
    float fadeGain = 1.0f;
    float fadeStep = 1.0f;
    int masterMuteSamples = 0;
    std::array<PatchProgram*, 8> retireStash {};  // retire queue full (rare)
    vibesynth::MacroSmoother smoother;
    vibesynth::OutputStage outputStage;
    std::array<std::atomic<float>*, numMacros> macroParams {};
    std::vector<float> voiceSum;     // mono voice mix for one chunk
    std::vector<double> macroBlock;  // smoothed macros, numMacros per sample
    int chunkCapacity = 0;
    int guardMuteLength = 960;
    double currentSampleRate = 48000.0;
    uint64_t noteCounter = 0;

    // Worker -> audio, audio -> worker (SPEC 3: ownership moves only here).
    ProgramQueue<16> swapQueue;
    ProgramQueue<64> retireQueue;

    // buildMutex serialises everything that builds or frees programs off
    // the audio thread: the worker's compile + push and retire draining,
    // prepareToPlay, setStateInformation's pre-prepare path. Never taken on
    // the audio thread.
    std::mutex buildMutex;
    std::atomic<double> preparedRate { 0.0 };       // 0 until prepareToPlay
    int preparedBlockSize = 0;
    std::unique_ptr<vibesynth::Patch> pendingBuild;  // to compile in prepareToPlay
    PatchProgram* heldProgram = nullptr;            // swap queue was full; retried by the worker

    std::mutex jobMutex;
    std::deque<LoadJob> jobs;
    std::atomic<uint64_t> generation { 0 };        // bumped by every load/restore; newest wins
    std::atomic<bool> cancelValidation { false };  // a newer job or shutdown

    std::mutex resultMutex;
    std::vector<LoadResult> results;

    // What is (or is about to be) playing: saved by getStateInformation.
    mutable std::mutex patchMutex;
    vibesynth::Patch activePatch;
    vibesynth::Patch defaultPatch;  // reese.pulsar, also the restore fallback
    std::atomic<int> currentProgramIndex { 0 };

    // Message thread only.
    juce::String statusText;
    std::array<juce::String, numMacros> macroLabels;

    Worker worker { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VibeSynthAudioProcessor)
};
