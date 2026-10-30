#include "PluginProcessor.h"

#include <cmath>

namespace
{
// All MIDI notes trigger this sound.
class SineSound final : public juce::SynthesiserSound
{
public:
    bool appliesToNote (int) override    { return true; }
    bool appliesToChannel (int) override { return true; }
};

// Simple sine voice with a short click-free envelope.
//
// renderNextBlock ADDS into the output buffer, starting at startSample for
// numSamples. It never overwrites and never clears: the Synthesiser mixes all
// voices additively, and the processor clears only surplus channels.
class SineVoice final : public juce::SynthesiserVoice
{
public:
    SineVoice()
    {
        // ~5 ms attack, no decay, full sustain, ~30 ms release: no clicks.
        adsr.setParameters ({ 0.005f, 0.0f, 1.0f, 0.03f });
    }

    bool canPlaySound (juce::SynthesiserSound* sound) override
    {
        return dynamic_cast<SineSound*> (sound) != nullptr;
    }

    void startNote (int midiNoteNumber, float velocity,
                    juce::SynthesiserSound*, int /*currentPitchWheelPosition*/) override
    {
        currentAngle = 0.0;
        level = velocity;
        angleDelta = juce::MathConstants<double>::twoPi
                   * juce::MidiMessage::getMidiNoteInHertz (midiNoteNumber)
                   / getSampleRate();

        adsr.reset();
        adsr.noteOn();
    }

    void stopNote (float /*velocity*/, bool allowTailOff) override
    {
        if (allowTailOff)
        {
            adsr.noteOff();
        }
        else
        {
            clearCurrentNote();
            adsr.reset();
        }
    }

    using SynthesiserVoice::renderNextBlock;

    void pitchWheelMoved (int) override      {}
    void controllerMoved (int, int) override {}

    void setCurrentPlaybackSampleRate (double newRate) override
    {
        SynthesiserVoice::setCurrentPlaybackSampleRate (newRate);
        adsr.setSampleRate (newRate);
    }

    void renderNextBlock (juce::AudioBuffer<float>& outputBuffer,
                          int startSample, int numSamples) override
    {
        if (! isVoiceActive())
            return;

        constexpr double twoPi = juce::MathConstants<double>::twoPi;

        const int numChannels = outputBuffer.getNumChannels();
        int sampleIndex = startSample;
        int samplesLeft = numSamples;

        while (samplesLeft > 0)
        {
            if (! adsr.isActive())
            {
                clearCurrentNote();
                break;
            }

            const float envValue = adsr.getNextSample();
            const float sample = static_cast<float> (std::sin (currentAngle)) * level * envValue;

            currentAngle += angleDelta;
            if (currentAngle >= twoPi)
                currentAngle -= twoPi;

            for (int ch = 0; ch < numChannels; ++ch)
                outputBuffer.addSample (ch, sampleIndex, sample);

            ++sampleIndex;
            --samplesLeft;
        }
    }

private:
    juce::ADSR adsr;
    double currentAngle = 0.0;
    double angleDelta = 0.0;
    float level = 0.0f;
};
} // namespace

//==============================================================================
VibeSynthAudioProcessor::VibeSynthAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VibeSynthParameters", createParameterLayout())
{
    for (int i = 0; i < numVoices; ++i)
        synth.addVoice (new SineVoice());

    synth.addSound (new SineSound());
}

VibeSynthAudioProcessor::~VibeSynthAudioProcessor() = default;

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout VibeSynthAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (int i = 1; i <= numMacros; ++i)
    {
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            "macro" + juce::String (i),
            "Macro " + juce::String (i),
            juce::NormalisableRange<float> (0.0f, 1.0f),
            0.5f));
    }

    return layout;
}

//==============================================================================
const juce::String VibeSynthAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool VibeSynthAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool VibeSynthAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool VibeSynthAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double VibeSynthAudioProcessor::getTailLengthSeconds() const
{
    // Covers the ~30 ms release envelope.
    return 0.05;
}


int VibeSynthAudioProcessor::getNumPrograms()
{
    return 1;
}

int VibeSynthAudioProcessor::getCurrentProgram()
{
    return 0;
}

void VibeSynthAudioProcessor::setCurrentProgram (int) {}

const juce::String VibeSynthAudioProcessor::getProgramName (int)
{
    return {};
}


//==============================================================================
void VibeSynthAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Voices use the rate for pitch and envelope timing. No fixed-size
    // buffers are derived from the block size, so any sample rate or block
    // size works (pluginval will exercise this in M2).
    juce::ignoreUnused (samplesPerBlock);
    synth.setCurrentPlaybackSampleRate (sampleRate);
}

void VibeSynthAudioProcessor::releaseResources()
{
    // Nothing to release: voices are pre-allocated in the constructor and
    // nothing is allocated per block.
}

bool VibeSynthAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
}

void VibeSynthAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                            juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // Clear surplus output channels only; the synth voices ADD their output.
    for (auto i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    synth.renderNextBlock (buffer, midiMessages, 0, buffer.getNumSamples());

    buffer.applyGain (juce::Decibels::decibelsToGain (outputGainDb));
}

//==============================================================================
bool VibeSynthAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* VibeSynthAudioProcessor::createEditor()
{
    // M1: JUCE's generic editor directly. The real UI arrives in M9.
    return new juce::GenericAudioProcessorEditor (*this);
}

//==============================================================================
void VibeSynthAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void VibeSynthAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VibeSynthAudioProcessor();
}

void VibeSynthAudioProcessor::changeProgramName (int, const juce::String&) {}
