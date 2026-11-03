#pragma once

// M14c: LLM judge for --batch evaluation (SPEC §13 item 14c).
// Measures audio descriptors from the offline riff phrase render,
// inspects voice oscillators and patch graph/macros, and sends
// an evaluation request to the LLM.

#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ai/AiSettings.h"
#include "ai/MetaClient.h"
#include "patch/Patch.h"

namespace vibesynth_cli {

// Audio descriptors measured from the rendered phrase.
struct AudioDescriptors
{
    double brightnessHz = 0.0;    // spectral centroid (Hz)
    double noisiness = 0.0;       // spectral flatness (Wiener entropy, 0..1)
    double movementPerSec = 0.0;  // spectral flux / change rate per second
    double attackMs = 0.0;        // attack time in milliseconds
    double subLevelPct = 0.0;     // energy below 100 Hz vs total energy (%)
    double stereoWidth = 0.0;     // side-to-mid RMS ratio (0 = mono, 1 = wide)

    std::string describe() const;
};

// Measures descriptors from an audio buffer.
AudioDescriptors measureDescriptors (const float* left, const float* right, int numSamples, double sampleRate);
AudioDescriptors measureDescriptors (const juce::AudioBuffer<float>& buffer, double sampleRate);

// Voice oscillator analysis.
struct OscAnalysis
{
    std::map<std::string, int> callCounts;
    std::string primaryOsc;  // main oscillator (e.g. "osc_square", "osc_supersaw", etc.)
    std::string summary;     // "osc_square (2), osc_sine (1)"
    bool isReeseLike = false;
};

// Extracts call counts of all oscillator functions in voice code.
OscAnalysis analyzeVoiceOscillators (const std::string& voiceCode);

// LLM judge score and explanation.
struct JudgeResult
{
    int score = 0;              // 1..5, or 0 if "no score"
    std::string reason;         // one-sentence reason from LLM (or "no score")
    int promptTokens = 0;
    int completionTokens = 0;
    double seconds = 0.0;
    bool malformed = false;
};

// Formats the judge prompt and evaluates the patch via LLM.
// If transport is provided, it is used instead of MetaClient::send (for tests).
JudgeResult runJudge (const std::string& userPrompt,
                      const vibesynth::Patch& patch,
                      const AudioDescriptors& desc,
                      const OscAnalysis& osc,
                      const vibesynth::AiConfig& cfg,
                      const std::string& apiKey,
                      const std::function<vibesynth::ChatReply (const vibesynth::ChatRequest&)>& transport = {});

// Helper to parse the judge's JSON response {"score": 1-5, "reason": "..."}.
// Returns true if parsing succeeded and score is within 1..5.
bool parseJudgeReply (const std::string& reply, int& score, std::string& reason);

} // namespace vibesynth_cli
