#include "PluginProcessor.h"

#include <algorithm>

// M4: 10 EEL2 voices + 1 master VM running the hardcoded test preset
// (engine/TestPreset.h). Everything that allocates or compiles happens in
// prepareToPlay; processBlock only renders.
//
// Voice stealing (SPEC 9.1, amended): at most 8 voices SOUND at once
// (Active, or Fading with a queued note). A 9th note-on gives the steal
// victim (the oldest released voice, else the oldest held one) a 2 ms
// linear fade and starts the new note at once on a free spare. Only when no spare is free is a voice hard-stolen: it fades for
// 2 ms in place, then its rack is reset and the queued note starts. A voice
// is freed by VoiceLifetime (-80 dB for 50 ms after gate-off, 10 s cap) or
// when its steal fade ends.
//
// Real-time rules (SPEC 11): processBlock performs no allocation, locks,
// logging, I/O or exceptions. MIDI is parsed from raw bytes (no MidiMessage
// copies).

namespace
{
uint32_t voiceSeed (int index) noexcept
{
    return 0x1000u + static_cast<uint32_t> (index) * 0x9E3779B1u;
}
} // namespace

//==============================================================================
VibeSynthAudioProcessor::VibeSynthAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VibeSynthParameters", createParameterLayout())
{
    for (int i = 0; i < numMacros; ++i)
        macroParams[(size_t) i] = apvts.getRawParameterValue ("macro" + juce::String (i + 1));
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
    // The test preset's release reaches -80 dB in about 0.45 s.
    return 0.5;
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
    // Off the audio thread: compile every VM and size every buffer here.
    // Any sample rate or block size works (pluginval exercises both); hosts
    // that send larger blocks than announced are handled in chunks.
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    chunkCapacity = samplesPerBlock > 0 ? samplesPerBlock : 512;
    voiceSum.assign ((size_t) chunkCapacity, 0.0f);
    macroBlock.assign ((size_t) chunkCapacity * (size_t) numMacros, 0.0);
    outputGain = juce::Decibels::decibelsToGain (outputGainDb);

    for (int i = 0; i < numVoiceObjects; ++i)
    {
        auto& v = voices[(size_t) i];
        v = VoiceSlot();
        v.eel = std::make_unique<vibesynth::EelVoice> (vibesynth::kTestPreset,
                                                       currentSampleRate, voiceSeed (i));
        if (! v.eel->isValid() && i == 0)
            juce::Logger::writeToLog ("Pulsar: EEL2 voice program failed: "
                                      + juce::String (v.eel->compileError()));
    }

    master = std::make_unique<vibesynth::EelMaster> (vibesynth::kTestPreset, currentSampleRate);
    if (! master->isValid())
        juce::Logger::writeToLog ("Pulsar: EEL2 master program failed: "
                                  + juce::String (master->compileError()));

    float targets[numMacros] = {};
    for (int m = 0; m < numMacros; ++m)
        targets[m] = macroParams[(size_t) m] != nullptr ? macroParams[(size_t) m]->load() : 0.5f;
    smoother.init (currentSampleRate);
    smoother.snapAll (targets);
    noteCounter = 0;
}

void VibeSynthAudioProcessor::releaseResources()
{
    // VMs and buffers stay allocated until the next prepareToPlay rebuilds
    // them (or the processor is destroyed).
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

//==============================================================================
// Voice management (audio thread; no allocation).

VibeSynthAudioProcessor::VoiceSlot* VibeSynthAudioProcessor::findFree() noexcept
{
    for (auto& v : voices)
        if (v.state == VoiceSlot::State::Free && v.eel != nullptr)
            return &v;
    return nullptr;
}

VibeSynthAudioProcessor::VoiceSlot*
VibeSynthAudioProcessor::oldestWhere (bool (*pred) (const VoiceSlot&)) noexcept
{
    VoiceSlot* best = nullptr;
    for (auto& v : voices)
        if (v.eel != nullptr && pred (v) && (best == nullptr || v.startOrder < best->startOrder))
            best = &v;
    return best;
}

VibeSynthAudioProcessor::VoiceSlot* VibeSynthAudioProcessor::stealVictim() noexcept
{
    // A note-off still waiting for its note-on sample counts as released.
    if (auto* v = oldestWhere ([] (const VoiceSlot& s)
                               { return s.state == VoiceSlot::State::Active
                                        && (! s.eel->lifetime.gateHeld || s.releasePending); }))
        return v;
    return oldestWhere ([] (const VoiceSlot& s) { return s.state == VoiceSlot::State::Active; });
}

void VibeSynthAudioProcessor::beginNote (VoiceSlot& v, int note, double velocity) noexcept
{
    v.state = VoiceSlot::State::Active;
    v.note = note;
    v.freq = juce::MidiMessage::getMidiNoteInHertz (note);
    v.vel = velocity;
    v.startOrder = ++noteCounter;
    v.renderedSinceNoteOn = false;
    v.releasePending = false;
    v.hasPending = false;
    v.pendingReleased = false;
    v.eel->noteOn (v.freq, static_cast<double> (note), velocity);
}

void VibeSynthAudioProcessor::beginFade (VoiceSlot& v) noexcept
{
    v.state = VoiceSlot::State::Fading;
    v.releasePending = false;
    v.eel->lifetime.startStealFade (currentSampleRate);
}

void VibeSynthAudioProcessor::startNote (int note, double velocity) noexcept
{
    using State = VoiceSlot::State;

    int sounding = 0;
    for (const auto& v : voices)
        if (v.state == State::Active || (v.state == State::Fading && v.hasPending))
            ++sounding;

    if (sounding >= maxSoundingVoices)
        if (auto* victim = stealVictim())
            beginFade (*victim);

    if (auto* spare = findFree())
    {
        beginNote (*spare, note, velocity);
        return;
    }

    // No spare: hard steal. Prefer a voice that is already fading with
    // nothing queued (its fade is shorter), else fade the steal victim in
    // place. With every voice fading and queued (10 note-ons within 2 ms)
    // the oldest queued note is replaced.
    VoiceSlot* target = oldestWhere ([] (const VoiceSlot& s)
                                     { return s.state == State::Fading && ! s.hasPending; });
    if (target == nullptr)
    {
        target = stealVictim();
        if (target != nullptr)
            beginFade (*target);
    }
    if (target == nullptr)
        target = oldestWhere ([] (const VoiceSlot& s) { return s.state == State::Fading; });
    if (target == nullptr)
        return;

    target->hasPending = true;
    target->pendingNote = note;
    target->pendingVel = velocity;
    target->pendingReleased = false;
}

void VibeSynthAudioProcessor::stopNote (int note) noexcept
{
    for (auto& v : voices)
    {
        if (v.state == VoiceSlot::State::Active && v.note == note && v.eel->lifetime.gateHeld)
        {
            // The patch must see gate = 1 for at least the note-on sample,
            // or its envelope never gets a rising edge.
            if (v.renderedSinceNoteOn)
                v.eel->lifetime.noteOff();
            else
                v.releasePending = true;
        }
        else if (v.state == VoiceSlot::State::Fading && v.hasPending && v.pendingNote == note)
        {
            v.pendingReleased = true;
        }
    }
}

void VibeSynthAudioProcessor::allNotesOff() noexcept
{
    for (auto& v : voices)
    {
        if (v.state == VoiceSlot::State::Active && v.eel->lifetime.gateHeld)
        {
            if (v.renderedSinceNoteOn)
                v.eel->lifetime.noteOff();
            else
                v.releasePending = true;
        }
        v.pendingReleased = true;
    }
}

void VibeSynthAudioProcessor::allSoundOff() noexcept
{
    for (auto& v : voices)
    {
        if (v.state == VoiceSlot::State::Active)
            beginFade (v);
        v.hasPending = false;
    }
}

void VibeSynthAudioProcessor::handleMidi (const juce::uint8* data, int numBytes) noexcept
{
    if (data == nullptr || numBytes < 3)
        return;
    const int type = data[0] & 0xF0;
    const int d1 = data[1] & 0x7F;
    const int d2 = data[2] & 0x7F;

    if (type == 0x90 && d2 > 0)
        startNote (d1, static_cast<double> (d2) / 127.0);
    else if (type == 0x80 || type == 0x90)
        stopNote (d1);
    else if (type == 0xB0 && d1 == 120)
        allSoundOff();
    else if (type == 0xB0 && d1 == 123)
        allNotesOff();
}

void VibeSynthAudioProcessor::renderVoices (int start, int end) noexcept
{
    for (auto& v : voices)
    {
        if (v.state == VoiceSlot::State::Free || v.eel == nullptr)
            continue;

        for (int i = start; i < end; ++i)
        {
            auto& lt = v.eel->lifetime;
            const double* macros = macroBlock.data() + (size_t) i * (size_t) numMacros;
            const float s = v.eel->renderSample (v.freq, lt.gateHeld ? 1.0 : 0.0,
                                                 static_cast<double> (v.note), v.vel, macros);
            voiceSum[(size_t) i] += s * lt.fadeGain();
            v.renderedSinceNoteOn = true;
            if (v.releasePending)
            {
                v.releasePending = false;
                lt.noteOff();
            }

            if (! lt.track (s, currentSampleRate))
                continue;

            if (v.state == VoiceSlot::State::Fading && v.hasPending)
            {
                // Hard steal: the fade has reached 0. Reset the rack so the
                // patch's envelopes restart from 0 (no jump), then start the
                // queued note in place on the next sample.
                const bool released = v.pendingReleased;
                v.eel->rack().reset();
                beginNote (v, v.pendingNote, v.pendingVel);
                v.releasePending = released;
                continue;
            }

            v.state = VoiceSlot::State::Free;
            v.hasPending = false;
            break;
        }
    }
}

void VibeSynthAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                            juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    buffer.clear();

    if (chunkCapacity <= 0 || master == nullptr || numChannels <= 0)
        return;

    float targets[numMacros] = {};
    for (int m = 0; m < numMacros; ++m)
        targets[m] = macroParams[(size_t) m] != nullptr
                         ? macroParams[(size_t) m]->load (std::memory_order_relaxed)
                         : 0.5f;

    float* outL = buffer.getWritePointer (0);
    float* outR = numChannels > 1 ? buffer.getWritePointer (1) : nullptr;

    auto midiIt = midiMessages.cbegin();
    const auto midiEnd = midiMessages.cend();

    for (int chunkStart = 0; chunkStart < numSamples; chunkStart += chunkCapacity)
    {
        const int n = std::min (chunkCapacity, numSamples - chunkStart);

        for (int i = 0; i < n; ++i)
        {
            smoother.step (targets);
            std::copy (smoother.current, smoother.current + numMacros,
                       macroBlock.data() + (size_t) i * (size_t) numMacros);
        }
        std::fill (voiceSum.begin(), voiceSum.begin() + n, 0.0f);

        // Sample-accurate MIDI: render up to each event, then apply it.
        int pos = 0;
        for (; midiIt != midiEnd; ++midiIt)
        {
            const auto event = *midiIt;
            const int at = std::max (event.samplePosition - chunkStart, pos);
            if (at >= n)
                break;
            if (at > pos)
            {
                renderVoices (pos, at);
                pos = at;
            }
            handleMidi (event.data, event.numBytes);
        }
        if (pos < n)
            renderVoices (pos, n);

        for (int i = 0; i < n; ++i)
        {
            const float in = voiceSum[(size_t) i];
            float l = 0.0f, r = 0.0f;
            master->renderSample (in, in, l, r,
                                  macroBlock.data() + (size_t) i * (size_t) numMacros);
            l *= outputGain;
            r *= outputGain;
            if (outR != nullptr)
            {
                outL[chunkStart + i] = l;
                outR[chunkStart + i] = r;
            }
            else
            {
                outL[chunkStart + i] = 0.5f * (l + r);
            }
        }
    }

    // Events stamped past the block end still apply (no stuck notes).
    for (; midiIt != midiEnd; ++midiIt)
    {
        const auto event = *midiIt;
        handleMidi (event.data, event.numBytes);
    }
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
