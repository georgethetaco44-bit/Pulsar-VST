// M6: vibesynth_cli --render (SPEC §10.3). Plays a fixed test phrase
// through vibesynth::PatchPlayer, the plugin's own chain (voice allocation
// and stealing, sample-accurate MIDI, macro smoothing, master VM, runtime
// guard, trim -> DC block -> limiter -> clip), in 512-sample blocks like a
// host, and writes a 24-bit stereo WAV with juce::WavAudioFormat.
//
// The patch is loaded as the plugin loads it: a .pulsar file runs the
// offline test and uses the computed trim; a built-in (file stem such as
// "reese" or its name such as "Clean Sub", any case) compiles with its
// stored trim.
//
// The phrase (48 kHz default, 6.5 s + tail, velocity 100, macros at the
// patch defaults unless --macro overrides them, --bpm 140 by default with
// beat 0 at the start, free-running like a stopped host):
//   0.0-2.0 s  one bar of bass: notes 28, 31, 35, 40, 0.45 s each
//   2.5-4.0 s  a held chord: 48, 52, 55
//   4.5-6.5 s  note 40 held while macro 1 sweeps 0 -> 1 (then stays at 1)
//   then --seconds-tail (default 1.5 s) of release tail.

#include "Render.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "patch/Patch.h"
#include "patch/PatchPlayer.h"
#include "patch/PatchValidator.h"

namespace vibesynth_cli {

namespace {

using vibesynth::PatchPlayer;

constexpr int kBlock = 512;      // host block size
constexpr int kVelocity = 100;

struct PhraseNote
{
    double on;
    double off;
    int note;
};

constexpr PhraseNote kPhrase[] = {
    { 0.0, 0.45, 28 }, { 0.5, 0.95, 31 }, { 1.0, 1.45, 35 }, { 1.5, 1.95, 40 },
    { 2.5, 4.0, 48 },  { 2.5, 4.0, 52 },  { 2.5, 4.0, 55 },
    { 4.5, 6.5, 40 },
};
constexpr double kSweepStart = 4.5;
constexpr double kSweepEnd = 6.5;
constexpr double kPhraseEnd = 6.5;

// One MIDI event, shaped like juce::MidiMessageMetadata for
// PatchPlayer::renderChunk (samplePosition, data, numBytes).
struct Event
{
    int samplePosition;
    const uint8_t* data;
    int numBytes;
};

// "neuro_growl", "Neuro Growl" or "neuro_growl.pulsar" -> built-in index.
int findBuiltin (const juce::String& arg)
{
    const juce::String want = arg.endsWithIgnoreCase (".pulsar") ? arg.dropLastCharacters (7) : arg;
    for (int i = 0; i < vibesynth::numBuiltinPresets(); ++i)
    {
        const juce::String stem = juce::String (vibesynth::builtinPresetFile (i)).upToLastOccurrenceOf (".", false, false);
        if (want.equalsIgnoreCase (stem) || want.equalsIgnoreCase (juce::String (vibesynth::builtinPresetName (i))))
            return i;
    }
    return -1;
}

double dbfs (double v)
{
    return v > 0.0 ? 20.0 * std::log10 (v) : -150.0;
}

} // namespace

int runRender (int argc, char** argv, double cpuScale)
{
    if (argc < 2)
        return 2;
    const juce::String patchArg (argv[0]);
    const juce::File outFile = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (argv[1]));
    double sr = 48000.0;
    double tail = 1.5;
    double bpm = 140.0;
    std::vector<std::pair<int, double>> macroOverrides;
    for (int i = 2; i < argc; ++i)
    {
        const std::string opt = argv[i];
        const bool hasValue = i + 1 < argc;
        if (opt == "--sr" && hasValue)
            sr = std::atof (argv[++i]);
        else if (opt == "--seconds-tail" && hasValue)
            tail = std::atof (argv[++i]);
        else if (opt == "--bpm" && hasValue)
            bpm = std::atof (argv[++i]);
        else if (opt == "--macro" && hasValue)
        {
            const juce::String kv (argv[++i]);
            const int n = kv.upToFirstOccurrenceOf ("=", false, false).getIntValue();
            if (! kv.containsChar ('=') || n < 1 || n > PatchPlayer::kNumMacros)
            {
                std::printf ("bad --macro %s (want N=V, N 1-8)\n", kv.toRawUTF8());
                return 2;
            }
            macroOverrides.emplace_back (n - 1, juce::jlimit (0.0, 1.0, kv.fromFirstOccurrenceOf ("=", false, false).getDoubleValue()));
        }
        else
        {
            std::printf ("unknown render option %s\n", opt.c_str());
            return 2;
        }
    }
    if (! (sr >= 8000.0 && sr <= 384000.0) || ! (tail >= 0.0 && tail <= 60.0)
        || ! (bpm >= 20.0 && bpm <= 999.0))
    {
        std::printf ("bad --sr, --seconds-tail or --bpm\n");
        return 2;
    }

    // Load as the plugin does: files get the offline test, built-ins don't.
    std::string text;
    bool builtin = false;
    const juce::File patchFile = juce::File::getCurrentWorkingDirectory().getChildFile (patchArg);
    if (patchFile.existsAsFile())
    {
        text = patchFile.loadFileAsString().toStdString();
    }
    else
    {
        const int index = findBuiltin (patchArg);
        if (index < 0)
        {
            std::printf ("no file or built-in preset named \"%s\"\n", patchArg.toRawUTF8());
            return 2;
        }
        text = vibesynth::builtinPresetText (index);
        builtin = true;
    }

    const vibesynth::ParseResult pr = vibesynth::parsePatch (text);
    if (! pr.ok())
    {
        std::printf ("REJECTED (%s): %s\n", vibesynth::patchStatusName (pr.status), pr.error.c_str());
        return 1;
    }
    vibesynth::Patch patch = pr.patch;
    if (! builtin)
    {
        const vibesynth::ValidationResult vr = vibesynth::validatePatch (patch, sr, nullptr, cpuScale);
        if (! vr.ok())
        {
            std::printf ("REJECTED (%s): %s\n", vibesynth::patchStatusName (vr.status), vr.error.c_str());
            return 1;
        }
        patch.trim = vr.trim;
    }
    std::string error;
    const auto program = vibesynth::PatchProgram::build (patch, sr, error);
    if (program == nullptr)
    {
        std::printf ("REJECTED: %s\n", error.c_str());
        return 1;
    }

    float targets[PatchPlayer::kNumMacros] = {};
    for (int m = 0; m < PatchPlayer::kNumMacros; ++m)
        targets[m] = static_cast<float> (patch.macroDefault (m));
    for (const auto& o : macroOverrides)
        targets[o.first] = static_cast<float> (o.second);

    // The phrase as raw MIDI, sorted by time (note-offs first on a tie).
    std::vector<std::array<uint8_t, 3>> bytes;
    std::vector<std::pair<int, size_t>> order;
    for (const PhraseNote& n : kPhrase)
    {
        order.emplace_back ((int) std::lround (n.off * sr), bytes.size());
        bytes.push_back ({ 0x80, (uint8_t) n.note, 0 });
        order.emplace_back ((int) std::lround (n.on * sr), bytes.size());
        bytes.push_back ({ 0x90, (uint8_t) n.note, (uint8_t) kVelocity });
    }
    std::stable_sort (order.begin(), order.end(), [&] (const auto& a, const auto& b)
                      { return a.first < b.first
                            || (a.first == b.first && bytes[a.second][0] < bytes[b.second][0]); });
    std::vector<Event> events;
    for (const auto& o : order)
        events.push_back ({ o.first, bytes[o.second].data(), 3 });

    PatchPlayer player;
    player.setProgram (program.get());
    player.prepare (sr, kBlock, targets);
    player.setHostTransport (bpm, false, false, 0.0); // beat runs on from 0

    const int total = (int) std::lround ((kPhraseEnd + tail) * sr);
    juce::AudioBuffer<float> out (2, total);
    auto it = events.cbegin();
    const auto end = events.cend();
    {
        juce::ScopedNoDenormals noDenormals; // as processBlock
        for (int start = 0; start < total; start += kBlock)
        {
            const int n = std::min (kBlock, total - start);
            // Host automation: macro 1 ramps 0 -> 1 across the held note,
            // one value per block.
            const double t = start / sr;
            if (t >= kSweepStart)
                targets[0] = static_cast<float> (std::min (1.0, (t - kSweepStart) / (kSweepEnd - kSweepStart)));
            player.renderChunk (n, targets, it, end, start);
            out.copyFrom (0, start, player.outL(), n);
            out.copyFrom (1, start, player.outR(), n);
        }
    }

    const double peak = std::max (out.getMagnitude (0, 0, total), out.getMagnitude (1, 0, total));
    const double rms = std::sqrt (0.5 * (std::pow (out.getRMSLevel (0, 0, total), 2.0)
                                         + std::pow (out.getRMSLevel (1, 0, total), 2.0)));

    outFile.getParentDirectory().createDirectory();
    outFile.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (outFile);
    if (! static_cast<juce::FileOutputStream&> (*stream).openedOk())
    {
        std::printf ("cannot write %s\n", outFile.getFullPathName().toRawUTF8());
        return 1;
    }
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                   .withSampleRate (sr)
                                                   .withNumChannels (2)
                                                   .withBitsPerSample (24));
    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (out, 0, total))
    {
        std::printf ("cannot write %s\n", outFile.getFullPathName().toRawUTF8());
        return 1;
    }
    writer.reset(); // flushes and closes the file

    std::printf ("rendered %s (%s, trim %+.1f dB) -> %s: %.2f s at %.0f Hz, %.0f bpm, "
                 "24-bit stereo, peak %+.1f dBFS, RMS %+.1f dBFS\n",
                 patch.name.c_str(), builtin ? "built-in, stored trim" : "file, computed trim",
                 dbfs (patch.trim), outFile.getFullPathName().toRawUTF8(), total / sr, sr, bpm,
                 dbfs (peak), dbfs (rms));
    return 0;
}

} // namespace vibesynth_cli
