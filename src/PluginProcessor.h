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

#include "ai/AiSettings.h"
#include "ai/Generator.h"
#include "engine/NodeMeters.h"
#include "patch/Patch.h"
#include "patch/PatchEngine.h"
#include "patch/PatchPlayer.h"
#include "patch/PatchValidator.h"

//==============================================================================
/**
    VibeSynthAudioProcessor, Milestone 7 (SPEC section 13, M7).

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
    - M6: voices, MIDI, the master VM, the runtime guard and the output
      stage live in vibesynth::PatchPlayer (shared with vibesynth_cli
      --render). Its voice slot state (note, gate, steal fade) survives a
      swap. At most 8 voices sound at once; the 2 spares let a stolen voice
      fade out over 2 ms while the new note starts immediately.
    - M7: voices run at 2x the host rate with one half-band decimator on
      their sum (SPEC 4.2); "Voice Mode" (Patch / Poly / Mono) and "Glide"
      host parameters after the macros, pitch bend and the mod wheel (`mw`)
      (SPEC 9.1), all in the player.
    - Runtime guard (SPEC 7.2): a voice that outputs a non-finite sample is
      zeroed, reset (rack + VM variables + mem[]) and muted for 20 ms; the
      master gets the same treatment. Output chain: trim -> DC block ->
      limiter (-0.3 dBFS) -> clip -> final non-finite check.
    - State (SPEC 9.3): the APVTS tree plus the patch JSON as a property.
      Restoring recompiles the saved code (no offline matrix).
    - The built-in presets are the host programs. Selecting one compiles it
      with its stored trim (no matrix, SPEC 9.4); CI's --selftest checks
      that every stored trim matches the computed one.
    - M8 (SPEC 6): generate() hands a prompt to a second worker thread that
      runs vibesynth::Generator (prompt -> request -> parse -> lint ->
      compile -> offline test, self-heal <= 2 retries) and passes a passing
      patch to the load worker, which swaps it in like any load (macro
      defaults, labels). One generation at a time: a new one cancels the
      one in flight. A failed generation leaves the current patch playing.
      Progress goes to the status line. No network activity happens until
      generate() is called (none in the constructor or prepareToPlay).
    - M9 (SPEC §9.5, §9.6): what the editor shows lives here, so the editor
      can close and reopen at any time (even mid-generation): the status
      (kind + text + detail), the generation stage and start time, the
      active patch and where it came from, and its node list. Nodes come
      from the offline test (file loads, generations) or, for compile-only
      loads, from a short silent probe render on the load worker when it
      is idle (only once an editor has been opened). Every block the audio thread moves the racks' meter peaks
      into a NodeMeterBank and the output peaks into two atomics; the
      editor takes them at 30 fps and never touches a program. Every
      successful generation is saved to the history folder (newest 50).
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
    // A file load runs the full compile + offline test on the worker; a
    // built-in only compiles (stored trim). A rejected patch leaves the
    // current sound playing and the reason in the status.
    void loadBuiltin (int index);
    void loadPatchFile (const juce::File& file);
    // The active patch as .pulsar text (for Save...).
    std::string getActivePatchText() const;
    juce::String getActivePatchName() const;
    // Message thread only (updated before sendChangeMessage()).
    enum class StatusKind { Info, Busy, Success, Error };
    struct Status
    {
        StatusKind kind = StatusKind::Info;
        juce::String text;    // one line
        juce::String detail;  // the full reason (errors), or empty
    };
    const Status& getStatus() const noexcept { return status; }
    juce::String getMacroLabel (int index) const;
    // Message thread: sets the status line and notifies the editor.
    void setStatus (const juce::String& text, StatusKind kind = StatusKind::Info,
                    const juce::String& detail = {});

    // M9 editor state (message thread). The patch the UI shows (bumped
    // version on every successful load), where it came from ("builtin:3",
    // or a file's full path), and its node list (known once the validator
    // or the probe has run; the version bumps on every change).
    const vibesynth::Patch& getUiPatch() const noexcept { return uiPatch; }
    // The editor saved the active patch (new name, macro defaults) to
    // `file`: it becomes the active patch's source and what state saves.
    // Same code, so nothing is recompiled.
    void adoptSavedPatch (const juce::File& file, const vibesynth::Patch& patch);
    int getPatchVersion() const noexcept { return patchVersion; }
    juce::String getActiveSource() const { return activeSource; }
    const vibesynth::TouchedNodes& getActiveNodes() const noexcept { return uiNodes; }
    // The editor calls this when it opens: compile-only loads only probe
    // for their nodes once an editor has wanted them (no CPU otherwise).
    void wantNodes()
    {
        nodesWanted = true;
        worker.notify();
    }
    bool areNodesKnown() const noexcept { return nodesKnown; }
    int getNodesVersion() const noexcept { return nodesVersion; }
    int getHistoryVersion() const noexcept { return historyVersion.load(); }
    // The running generation's stage ("Waiting for model") and seconds
    // since generate().
    juce::String getGenerationStage() const { return generationStage; }
    double getGenerationSeconds() const;

    // M9 meters (SPEC §9.6, §11): written by the audio thread once per
    // block, taken by the editor (relaxed atomics only).
    vibesynth::NodeMeterBank& nodeMeters() noexcept { return meterBank; }
    float takeOutputPeak (int channel) noexcept
    {
        return vibesynth::NodeMeterBank::take (outputPeak[channel != 0 ? 1 : 0]);
    }
    void publishOutputPeak (int channel, float peak) noexcept
    {
        vibesynth::NodeMeterBank::raise (outputPeak[channel != 0 ? 1 : 0], peak);
    }

    // M9: the editor's size as a scale of 960 x 640, kept across editor
    // reopenings and saved with the state.
    float getEditorScale() const noexcept { return editorScale.load(); }
    void setEditorScale (float s) noexcept { editorScale = s; }

    //==============================================================================
    // M8 generation (message thread). generate() cancels any generation in
    // flight; without an API key it only sets the status.
    void generate (const juce::String& prompt);
    void cancelGeneration();
    bool isGenerating() const noexcept { return generating.load() || generationPending.load(); }
    bool canGenerate() { return aiSettings().hasKey(); }
    // The settings (read from disk on first use, never in the constructor).
    vibesynth::AiSettings& aiSettings();

    // M9 onboarding: the settings panel's "Test key" (message thread). One
    // tiny request on a helper thread (never in a constructor or
    // prepareToPlay); the result arrives through ChangeBroadcaster. A newer
    // test or clearKeyTest() makes an older reply stale.
    struct KeyTest
    {
        enum class State { Idle, Running, Ok, Failed };
        State state = State::Idle;
        juce::String text;   // "Key works" or the mapped error
        bool operator== (const KeyTest& o) const { return state == o.state && text == o.text; }
        bool operator!= (const KeyTest& o) const { return ! (*this == o); }
    };
    void testApiKey (const std::string& key, const vibesynth::AiConfig& config);
    void clearKeyTest();
    const KeyTest& getKeyTest() const noexcept { return keyTest; }

    //==============================================================================
    static constexpr int numMacros = vibesynth::MacroSmoother::kNumMacros;
    static constexpr const char* voiceModeParamId = "voiceMode";  // M7
    static constexpr const char* glideParamId = "glide";          // M7
    static constexpr double swapFadeSeconds = 0.005;   // SPEC 7.3

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
        // Generated (M9): the offline test's node list.
        bool hasNodes = false;
        vibesynth::TouchedNodes nodes;
        juce::String sourceKey;  // "builtin:N" or a file path (the editor's highlight)
        juce::String detail;     // Generated: attempts, time, notes
        // Load: parse + offline matrix + compile (files). Builtin: parse +
        // compile with the stored trim, sets macro defaults. Restore: parse
        // + compile, keeps macros, falls back to the default patch.
        // Generated (M8): a patch the generation worker already tested
        // (its trim set); parse + compile, sets macro defaults.
        enum class Kind { Load, Builtin, Restore, Generated };
        Kind kind = Kind::Load;
        std::string text;
        juce::String source;     // shown in the status line
        uint64_t generation = 0;
    };

    struct LoadResult
    {
        bool ok = false;
        juce::String message;
        juce::String detail;
        StatusKind kind = StatusKind::Info;
        juce::String sourceKey;
        bool hasNodes = false;
        bool nodesOnly = false;  // a probe result: only `nodes` (for `patch`'s code)
        vibesynth::TouchedNodes nodes;
        vibesynth::Patch patch;  // labels/defaults for the UI when ok
        bool setMacros = false;  // a Load or Builtin (not a restore) sets macros to the patch defaults
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

    // M8: runs one generation at a time (network + offline tests take
    // seconds, so it is separate from the load worker, which stays free
    // for preset changes meanwhile).
    class GenerationWorker final : public juce::Thread
    {
    public:
        explicit GenerationWorker (VibeSynthAudioProcessor& p)
            : juce::Thread ("Pulsar generation worker"), owner (p) {}
        void run() override;

    private:
        VibeSynthAudioProcessor& owner;
    };
    // One test at a time; started by testApiKey(), joined by the next test
    // or the destructor.
    class KeyTestThread final : public juce::Thread
    {
    public:
        KeyTestThread (VibeSynthAudioProcessor& p, std::string k, vibesynth::AiConfig c, int testId)
            : juce::Thread ("Pulsar key test"), owner (p), key (std::move (k)), config (std::move (c)), id (testId) {}
        void run() override;
        std::atomic<bool> cancel { false };

    private:
        VibeSynthAudioProcessor& owner;
        std::string key;
        vibesynth::AiConfig config;
        int id;
    };
    void postKeyTest (int id, const KeyTest& result);
    void runGeneration (const vibesynth::GenerationRequest& request);
    void postGenerationStatus (StatusKind kind, const juce::String& text, const juce::String& detail = {});
    void probeActivePatch();  // load worker, when idle and probeRequested

    void postJob (LoadJob job);
    bool takeJob (LoadJob& job);
    void processJob (const LoadJob& job);
    // Builds `patch` for the prepared rate and pushes it (worker, holding
    // buildMutex). Returns false with `error` set if it doesn't compile.
    bool buildAndPushLocked (const vibesynth::Patch& patch, std::string& error);
    void freeRetired() noexcept;  // caller holds buildMutex (single consumer)
    void postResult (LoadResult result);
    void handleAsyncUpdate() override;

    // Swap protocol (audio thread).
    void pollSwapQueue() noexcept;
    void swapPrograms() noexcept;
    void retire (PatchProgram* p) noexcept;
    void flushRetireStash() noexcept;
    int samplesUntilSilent() const noexcept;

    //==============================================================================
    // Audio-thread state. prepareToPlay and the destructor also touch it,
    // which is safe because the host never runs them concurrently with
    // processBlock. The player's program is the current sound (owned here).
    vibesynth::PatchPlayer player;
    PatchProgram* incoming = nullptr;  // received, waiting for the fade-out
    enum class Fade { None, Out, In };
    Fade fade = Fade::None;
    float fadeGain = 1.0f;
    float fadeStep = 1.0f;
    std::array<PatchProgram*, 8> retireStash {};  // retire queue full (rare)
    std::array<std::atomic<float>*, numMacros> macroParams {};
    std::atomic<float>* voiceModeParam = nullptr;
    std::atomic<float>* glideParam = nullptr;

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
    Status generationStatus;          // newest generation status (under resultMutex)
    bool generationStatusPending = false;
    std::atomic<bool> probeRequested { false };  // the active patch's nodes are unknown
    std::atomic<bool> nodesWanted { false };     // an editor has been opened

    // M8 generation: the request waiting to start (newest wins) and the
    // cancel flag for the one running.
    std::mutex generationMutex;
    std::unique_ptr<vibesynth::GenerationRequest> nextGeneration;
    std::atomic<bool> generationPending { false };
    std::atomic<bool> generating { false };
    std::atomic<bool> cancelGenerationFlag { false };

    // Key test: the running thread (message thread), the posted result
    // (under resultMutex) and what the UI shows (message thread).
    std::unique_ptr<KeyTestThread> keyTestThread;
    int keyTestId = 0;
    bool keyTestPosted = false;
    int postedKeyTestId = 0;
    KeyTest postedKeyTest;
    KeyTest keyTest;

    // What is (or is about to be) playing: saved by getStateInformation.
    mutable std::mutex patchMutex;
    vibesynth::Patch activePatch;
    vibesynth::Patch defaultPatch;  // reese.pulsar, also the restore fallback
    std::atomic<int> currentProgramIndex { 0 };

    // Message thread only.
    Status status;
    std::array<juce::String, numMacros> macroLabels;
    std::unique_ptr<vibesynth::AiSettings> settings;
    vibesynth::Patch uiPatch;
    int patchVersion = 0;
    juce::String activeSource { "builtin:0" };
    vibesynth::TouchedNodes uiNodes;
    bool nodesKnown = false;
    int nodesVersion = 0;
    juce::String generationStage;
    double generationStartMs = 0.0;

    // M9 meters and editor size (any thread; relaxed atomics).
    vibesynth::NodeMeterBank meterBank;
    std::array<std::atomic<float>, 2> outputPeak {};
    std::atomic<float> editorScale { 1.0f };
    std::atomic<int> historyVersion { 0 };  // bumped by the generation worker

    Worker worker { *this };
    GenerationWorker generationWorker { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VibeSynthAudioProcessor)
};
