#pragma once

// M6: vibesynth_cli --render (SPEC §10.3). See Render.cpp.

#include <juce_audio_basics/juce_audio_basics.h>
#include <string>
#include <utility>
#include <vector>

#include "patch/Patch.h"
#include "patch/PatchPlayer.h"

namespace vibesynth_cli {

// Returns true if the patch is classified as a bass (by prompt or name).
bool isBassPatch (const vibesynth::Patch& patch);

// Renders a patch into an AudioBuffer<float> using the same offline render path
// as --render (SPEC §10.3).
bool renderPatchToBuffer (const vibesynth::Patch& patch,
                          juce::AudioBuffer<float>& outBuffer,
                          double sr = 48000.0,
                          bool riff = true,
                          int oversampling = 2,
                          double bpm = 140.0,
                          double tail = 1.5,
                          int octave = 0,
                          vibesynth::PatchPlayer::VoiceMode mode = vibesynth::PatchPlayer::VoiceMode::Patch,
                          double glide = 0.3,
                          const std::vector<std::pair<int, double>>& macroOverrides = {});

bool renderPatchToBuffer (const vibesynth::Patch& patch,
                          juce::AudioBuffer<float>& outBuffer,
                          double sr,
                          const std::string& phrase,
                          int oversampling = 2,
                          double bpm = 140.0,
                          double tail = -1.0,
                          int octave = 0,
                          vibesynth::PatchPlayer::VoiceMode mode = vibesynth::PatchPlayer::VoiceMode::Patch,
                          double glide = 0.3,
                          const std::vector<std::pair<int, double>>& macroOverrides = {},
                          int holdBars = 4);

// args: <patch.pulsar | built-in name> <out.wav> [--sr HZ]
// [--seconds-tail S] [--bpm BPM] [--phrase default|riff|hold] [--hold-bars N]
// [--mode patch|poly|mono] [--glide G] [--macro N=V]... `oversampling` is
// the voice rate factor (vibesynth_cli --os, default 2). Returns 0 on
// success, 1 if the patch is rejected or the WAV can't be written, 2 on
// bad arguments.
int runRender (int argc, char** argv, int oversampling);

} // namespace vibesynth_cli
