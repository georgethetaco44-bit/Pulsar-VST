#include "PluginProcessor.h"

#include "patch/PatchEngine.h"

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

// M3: sine voice driven by an EEL2 VM. The per-sample DSP is the hardcoded
// program `out = osc_sine(0, freq);` — one native function call with the
// per-voice context (SPEC §4.2). ADSR and output level behaviour are
// unchanged from M1/M2; `out` is clamped and NaN/Inf-sanitised per SPEC §11.
//
// Threading: the EelVoice is compiled in the constructor (off the audio
// thread). The audio thread only runs renderSample() — no allocation,
// locks, or logging. A failed compile yields silence plus one logged line
// from prepareVoice (off-thread), never a crash.
class SineVoice final : public juce::SynthesiserVoice
{
public:
    SineVoice()
        : eel ("out = osc_sine(0, freq);")
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
        level = velocity;
        freqHz = juce::MidiMessage::getMidiNoteInHertz (midiNoteNumber);
        gateHeld = true;
        eel.resetOscillator();

        adsr.reset();
        adsr.noteOn();
    }

    void stopNote (float /*velocity*/, bool allowTailOff) override
    {
        gateHeld = false;
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
        eel.setSampleRate (newRate);
    }

    // Off audio thread: called from prepareToPlay. Reports a compile
    // failure exactly once per voice (logged, not thrown, not on-thread).
    void prepareVoice()
    {
        if (! eel.isValid() && ! compileErrorLogged)
        {
            compileErrorLogged = true;
            juce::Logger::writeToLog ("Pulsar: EEL2 voice program failed: "
                                      + juce::String (eel.compileError()));
        }
    }

    void renderNextBlock (juce::AudioBuffer<float>& outputBuffer,
                          int startSample, int numSamples) override
    {
        if (! isVoiceActive())
            return;

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
            // renderSample() clamps and sanitises `out` (SPEC §11); the
            // voice stays silent if its VM failed to compile.
            const float sample = eel.renderSample (freqHz, gateHeld ? 1.0 : 0.0)
                               * level * envValue;

            for (int ch = 0; ch < numChannels; ++ch)
                outputBuffer.addSample (ch, sampleIndex, sample);

            ++sampleIndex;
            --samplesLeft;
        }
    }

private:
    juce::ADSR adsr;
    vibesynth::EelVoice eel;
    double freqHz = 440.0;
    float level = 0.0f;
    bool gateHeld = false;
    bool compileErrorLogged = false;
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

    // M3: surface an EEL2 compile failure once, off the audio thread.
    for (int i = 0; i < synth.getNumVoices(); ++i)
        if (auto* v = dynamic_cast<SineVoice*> (synth.getVoice (i)))
            v->prepareVoice();
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
