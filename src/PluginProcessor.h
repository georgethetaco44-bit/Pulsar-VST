#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
/**
    VibeSynthVST Milestone 1 skeleton (SPEC section 13, M1).

    - 8 automatable float parameters "Macro 1".."Macro 8" (0..1, default 0.5)
      exposed via AudioProcessorValueTreeState. They are registered, automated
      and state-saved, but not yet wired to the sound (they drive EEL2 m1..m8
      from M3/M4 onwards).
    - 8-voice polyphonic sine synth (juce::Synthesiser) with a short attack
      and release so notes don't click. Output gain is -12 dB.
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
    static constexpr int numMacros = 8;
    static constexpr int numVoices = 8;
    static constexpr float outputGainDb = -12.0f;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::Synthesiser synth;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VibeSynthAudioProcessor)
};
