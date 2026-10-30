#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "patch/PatchEngine.h"

//==============================================================================
/**
    VibeSynthAudioProcessor, Milestone 4 (SPEC section 13, M4).

    - 8 automatable float parameters "Macro 1".."Macro 8" (0..1, default 0.5)
      exposed via AudioProcessorValueTreeState, smoothed per sample by
      vibesynth::MacroSmoother and bound to m1..m8 in every VM.
    - 10 EEL2 voice objects (vibesynth::EelVoice) plus one master VM
      (vibesynth::EelMaster), all compiled from the hardcoded test preset in
      prepareToPlay. At most 8 voices sound at once; the 2 spares let a
      stolen voice fade out over 2 ms while the new note starts immediately.
    - Own sample-accurate MIDI handling (no juce::Synthesiser). Voices add
      into a mono scratch buffer; the master VM turns it into stereo.
      Output gain is -12 dB.
*/
class VibeSynthAudioProcessor final : public juce::AudioProcessor
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
    static constexpr int numMacros = vibesynth::MacroSmoother::kNumMacros;
    static constexpr int maxSoundingVoices = 8;  // SPEC 9.1 polyphony
    static constexpr int numVoiceObjects = 10;   // + 2 spares for steal fades
    static constexpr float outputGainDb = -12.0f;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // One voice object. Active = playing (gate held or releasing); Fading =
    // stolen, in its 2 ms fade, optionally with a note queued to start in
    // place when the fade ends (hard steal).
    struct VoiceSlot
    {
        enum class State { Free, Active, Fading };

        std::unique_ptr<vibesynth::EelVoice> eel;
        State state = State::Free;
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
    };

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

    std::array<VoiceSlot, numVoiceObjects> voices;
    std::unique_ptr<vibesynth::EelMaster> master;
    vibesynth::MacroSmoother smoother;
    std::array<std::atomic<float>*, numMacros> macroParams {};

    // Allocated in prepareToPlay; processBlock never allocates.
    std::vector<float> voiceSum;     // mono voice mix for one chunk
    std::vector<double> macroBlock;  // smoothed macros, numMacros per sample
    int chunkCapacity = 0;

    double currentSampleRate = 48000.0;
    float outputGain = 1.0f;
    uint64_t noteCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VibeSynthAudioProcessor)
};
