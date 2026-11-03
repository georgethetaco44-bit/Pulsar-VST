// M14c: LLM judge for --batch evaluation (SPEC §13 item 14c).

#include "Judge.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <sstream>

#include "patch/PatchValidator.h"

namespace vibesynth_cli {

namespace {

constexpr double kPi = 3.14159265358979323846;

void radix2Fft (std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * kPi / static_cast<double> (len);
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

} // namespace

std::string AudioDescriptors::describe() const
{
    std::ostringstream ss;
    char buf[128];

    std::snprintf (buf, sizeof (buf), "- Brightness: %.0f Hz (spectral centroid)\n", brightnessHz);
    ss << buf;
    std::snprintf (buf, sizeof (buf), "- Noisiness: %.2f (spectral flatness, 0=tonal, 1=noise)\n", noisiness);
    ss << buf;
    std::snprintf (buf, sizeof (buf), "- Movement: %.2f spectral changes/sec\n", movementPerSec);
    ss << buf;
    std::snprintf (buf, sizeof (buf), "- Attack time: %.1f ms\n", attackMs);
    ss << buf;
    std::snprintf (buf, sizeof (buf), "- Sub level: %.1f%% of total energy below 100 Hz\n", subLevelPct);
    ss << buf;
    std::snprintf (buf, sizeof (buf), "- Stereo width: %.2f (0=mono, 1=wide)", stereoWidth);
    ss << buf;

    return ss.str();
}

AudioDescriptors measureDescriptors (const float* left, const float* right, int numSamples, double sampleRate)
{
    AudioDescriptors d;
    if (left == nullptr || numSamples < 1024 || sampleRate <= 0.0)
        return d;

    const bool stereo = (right != nullptr && right != left);

    // 1. Noisiness: M14a spectral flatness Wiener entropy (200 Hz - 8 kHz)
    d.noisiness = vibesynth::computeSpectralFlatness (left, right, numSamples, sampleRate);

    // 2. Attack time: onset to initial peak of the first note
    // Look at first 0.5 s or note
    const int lookAhead = std::min (numSamples, static_cast<int> (sampleRate * 0.5));
    float maxAmp = 0.0f;
    for (int i = 0; i < lookAhead; ++i)
    {
        const float amp = stereo ? 0.5f * (std::abs (left[i]) + std::abs (right[i])) : std::abs (left[i]);
        if (amp > maxAmp)
            maxAmp = amp;
    }

    if (maxAmp > 1.0e-4f)
    {
        int onsetSample = -1;
        int peakSample = -1;
        const float thresh10 = 0.10f * maxAmp;
        const float thresh90 = 0.90f * maxAmp;

        for (int i = 0; i < lookAhead; ++i)
        {
            const float amp = stereo ? 0.5f * (std::abs (left[i]) + std::abs (right[i])) : std::abs (left[i]);
            if (onsetSample < 0 && amp >= thresh10)
                onsetSample = i;
            if (peakSample < 0 && amp >= thresh90)
            {
                peakSample = i;
                break;
            }
        }
        if (onsetSample >= 0 && peakSample >= onsetSample)
            d.attackMs = 1000.0 * (peakSample - onsetSample) / sampleRate;
        else
            d.attackMs = 5.0;
    }

    // 3. Stereo width: RMS(Side) / RMS(Mid)
    double midEnergy = 0.0;
    double sideEnergy = 0.0;
    for (int i = 0; i < numSamples; ++i)
    {
        const double l = static_cast<double> (left[i]);
        const double r = stereo ? static_cast<double> (right[i]) : l;
        const double m = 0.5 * (l + r);
        const double s = 0.5 * (l - r);
        midEnergy += m * m;
        sideEnergy += s * s;
    }
    if (midEnergy > 1.0e-12)
        d.stereoWidth = std::clamp (std::sqrt (sideEnergy / midEnergy), 0.0, 2.0);
    else
        d.stereoWidth = 0.0;

    // 4. STFT for Spectral Centroid, Sub Level, and Movement (Spectral Flux)
    constexpr size_t N = 1024;
    constexpr size_t H = 512;
    const double binHz = sampleRate / static_cast<double> (N);

    std::vector<std::complex<double>> frame (N);
    std::vector<double> avgPower (N / 2 + 1, 0.0);
    std::vector<double> prevNormPower (N / 2 + 1, 0.0);

    double totalFlux = 0.0;
    int activeFrames = 0;
    int frameCount = 0;

    for (int start = 0; start + static_cast<int> (N) <= numSamples; start += H)
    {
        double frameEnergy = 0.0;
        for (size_t i = 0; i < N; ++i)
        {
            const double w = 0.5 * (1.0 - std::cos (2.0 * kPi * static_cast<double> (i) / static_cast<double> (N)));
            const double l = static_cast<double> (left[start + i]);
            const double r = stereo ? static_cast<double> (right[start + i]) : l;
            const double s = 0.5 * (l + r) * w;
            frame[i] = s;
            frameEnergy += s * s;
        }

        radix2Fft (frame);

        std::vector<double> power (N / 2 + 1, 0.0);
        double powerSum = 0.0;
        for (size_t k = 0; k <= N / 2; ++k)
        {
            power[k] = std::norm (frame[k]);
            powerSum += power[k];
            avgPower[k] += power[k];
        }
        ++frameCount;

        if (powerSum > 1.0e-8)
        {
            // Normalize power spectrum for spectral flux
            std::vector<double> normPower (N / 2 + 1, 0.0);
            for (size_t k = 0; k <= N / 2; ++k)
                normPower[k] = power[k] / powerSum;

            if (activeFrames > 0)
            {
                double flux = 0.0;
                for (size_t k = 1; k <= N / 2; ++k)
                    flux += std::abs (normPower[k] - prevNormPower[k]);
                totalFlux += flux;
            }
            prevNormPower = normPower;
            ++activeFrames;
        }
    }

    if (frameCount > 0)
    {
        double sumPower = 0.0;
        double weightedFreq = 0.0;
        double subPower = 0.0;

        for (size_t k = 0; k <= N / 2; ++k)
        {
            avgPower[k] /= static_cast<double> (frameCount);
            const double freq = static_cast<double> (k) * binHz;
            if (k > 0) // skip DC
            {
                sumPower += avgPower[k];
                weightedFreq += freq * avgPower[k];
                if (freq < 100.0)
                    subPower += avgPower[k];
            }
        }

        if (sumPower > 1.0e-12)
        {
            d.brightnessHz = weightedFreq / sumPower;
            d.subLevelPct = std::clamp (100.0 * (subPower / sumPower), 0.0, 100.0);
        }

        const double activeSec = static_cast<double> (activeFrames) * static_cast<double> (H) / sampleRate;
        if (activeSec > 0.05)
            d.movementPerSec = totalFlux / activeSec;
        else
            d.movementPerSec = 0.0;
    }

    return d;
}

AudioDescriptors measureDescriptors (const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    if (buffer.getNumSamples() == 0 || buffer.getNumChannels() == 0)
        return {};
    const float* left = buffer.getReadPointer (0);
    const float* right = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : left;
    return measureDescriptors (left, right, buffer.getNumSamples(), sampleRate);
}

OscAnalysis analyzeVoiceOscillators (const std::string& voiceCode)
{
    static const char* const kOscs[] = {
        "osc_supersaw", "osc_wt", "osc_fm", "osc_sync", "osc_square",
        "osc_saw", "osc_tri", "phys_resonator", "phys_string", "noise", "osc_sine"
    };

    OscAnalysis result;
    size_t i = 0;
    const size_t n = voiceCode.size();

    while (i < n)
    {
        if (voiceCode[i] == '/' && i + 1 < n && voiceCode[i + 1] == '/')
        {
            i += 2;
            while (i < n && voiceCode[i] != '\n') ++i;
            continue;
        }
        if (voiceCode[i] == '/' && i + 1 < n && voiceCode[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < n && ! (voiceCode[i] == '*' && voiceCode[i + 1] == '/')) ++i;
            if (i + 1 < n) i += 2;
            continue;
        }
        if (voiceCode[i] == '"')
        {
            ++i;
            while (i < n && voiceCode[i] != '"')
            {
                if (voiceCode[i] == '\\' && i + 1 < n) ++i;
                ++i;
            }
            if (i < n) ++i;
            continue;
        }
        if ((voiceCode[i] >= 'a' && voiceCode[i] <= 'z') || (voiceCode[i] >= 'A' && voiceCode[i] <= 'Z') || voiceCode[i] == '_')
        {
            const size_t start = i;
            while (i < n && ((voiceCode[i] >= 'a' && voiceCode[i] <= 'z') || (voiceCode[i] >= 'A' && voiceCode[i] <= 'Z')
                             || (voiceCode[i] >= '0' && voiceCode[i] <= '9') || voiceCode[i] == '_'))
                ++i;
            const std::string ident = voiceCode.substr (start, i - start);
            size_t j = i;
            while (j < n && (voiceCode[j] == ' ' || voiceCode[j] == '\t' || voiceCode[j] == '\r' || voiceCode[j] == '\n'))
                ++j;
            if (j < n && voiceCode[j] == '(')
            {
                for (const char* osc : kOscs)
                {
                    if (ident == osc)
                    {
                        result.callCounts[ident]++;
                        break;
                    }
                }
            }
            continue;
        }
        ++i;
    }

    if (result.callCounts.empty())
    {
        result.primaryOsc = "none";
        result.summary = "none";
        result.isReeseLike = false;
        return result;
    }

    // Determine primary oscillator
    // If oscillators other than osc_sine exist, pick the one with the highest count.
    // Ties are broken in order of kOscs (e.g. osc_supersaw > osc_saw).
    std::string topNonSine;
    int maxNonSineCount = 0;
    for (const char* osc : kOscs)
    {
        if (std::strcmp (osc, "osc_sine") == 0)
            continue;
        const auto it = result.callCounts.find (osc);
        if (it != result.callCounts.end() && it->second > maxNonSineCount)
        {
            maxNonSineCount = it->second;
            topNonSine = osc;
        }
    }

    if (! topNonSine.empty())
        result.primaryOsc = topNonSine;
    else
        result.primaryOsc = "osc_sine";

    // Format summary list sorted by count descending
    std::vector<std::pair<int, std::string>> sorted;
    for (const auto& [name, count] : result.callCounts)
        sorted.emplace_back (-count, name);
    std::stable_sort (sorted.begin(), sorted.end());

    std::string sumStr;
    for (size_t si = 0; si < sorted.size(); ++si)
    {
        if (si > 0) sumStr += ", ";
        sumStr += sorted[si].second + " (" + std::to_string (-sorted[si].first) + ")";
    }
    result.summary = sumStr;

    // Reese detection:
    // "whether the patch is 'reese-like' (osc_supersaw, or two detuned saws, as the main source)"
    const int supersawCount = result.callCounts.count ("osc_supersaw") ? result.callCounts.at ("osc_supersaw") : 0;
    const int sawCount = result.callCounts.count ("osc_saw") ? result.callCounts.at ("osc_saw") : 0;
    const int squareCount = result.callCounts.count ("osc_square") ? result.callCounts.at ("osc_square") : 0;
    const int fmCount = result.callCounts.count ("osc_fm") ? result.callCounts.at ("osc_fm") : 0;

    if (supersawCount > 0 && supersawCount >= squareCount && supersawCount >= fmCount)
        result.isReeseLike = true;
    else if (sawCount >= 2 && sawCount >= squareCount && sawCount >= fmCount)
        result.isReeseLike = true;
    else
        result.isReeseLike = false;

    return result;
}

bool parseJudgeReply (const std::string& reply, int& score, std::string& reason)
{
    score = 0;
    reason.clear();

    juce::String text (reply);
    text = text.trim();

    // Strip ```json ... ``` code fence if present
    if (text.startsWith ("```"))
    {
        text = text.fromFirstOccurrenceOf ("\n", false, false);
        if (text.endsWith ("```"))
            text = text.dropLastCharacters (3);
        text = text.trim();
    }

    const int firstBrace = text.indexOfChar ('{');
    const int lastBrace = text.lastIndexOfChar ('}');
    if (firstBrace < 0 || lastBrace <= firstBrace)
        return false;

    text = text.substring (firstBrace, lastBrace + 1);

    const auto parsed = juce::JSON::parse (text);
    if (! parsed.isObject())
        return false;

    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr)
        return false;

    if (! obj->hasProperty ("score") || ! obj->hasProperty ("reason"))
        return false;

    const auto scoreVar = obj->getProperty ("score");
    if (! scoreVar.isInt() && ! scoreVar.isInt64() && ! scoreVar.isDouble())
        return false;

    const int s = static_cast<int> (scoreVar);
    if (s < 1 || s > 5)
        return false;

    score = s;
    reason = obj->getProperty ("reason").toString().trim().toStdString();
    return true;
}

JudgeResult runJudge (const std::string& userPrompt,
                      const vibesynth::Patch& patch,
                      const AudioDescriptors& desc,
                      const OscAnalysis& osc,
                      const vibesynth::AiConfig& cfg,
                      const std::string& apiKey,
                      const std::function<vibesynth::ChatReply (const vibesynth::ChatRequest&)>& transport)
{
    JudgeResult res;

    std::string macrosStr;
    for (size_t m = 0; m < patch.macros.size(); ++m)
    {
        if (m > 0) macrosStr += ", ";
        macrosStr += "m" + std::to_string (m + 1) + ": " + (patch.macros[m].label.empty() ? "(unlabeled)" : patch.macros[m].label);
    }

    std::string edgesStr = patch.graphJson.empty() ? "(none)" : patch.graphJson;

    const std::string systemPrompt =
        "You are an expert synthesizer judge evaluating whether a generated patch matches the user's sound design prompt.\n"
        "You are given the user's prompt, patch name, macro labels, graph edges, main oscillator functions, and measured audio descriptors.\n"
        "Evaluate how well the patch matches the prompt in sound character, timbre, and acoustic role.\n"
        "Score criteria (1-5 integer):\n"
        "5: The patch clearly matches the prompt in sound character and timbre.\n"
        "4: Good match with minor differences.\n"
        "3: Partial match (e.g. general area fits but wrong oscillator or missing key movement).\n"
        "2: Weak match (misses essential sound requirements).\n"
        "1: Completely misses the prompt (e.g. requested a chime/bell/siren and got a generic bass).\n\n"
        "Answer with JSON only:\n"
        "{\"score\": 1-5, \"reason\": \"<one sentence>\"}";

    const std::string userContent =
        "User prompt: " + userPrompt + "\n"
        "Patch name: " + patch.name + "\n"
        "Voice mode: " + patch.voiceMode + "\n"
        "Macro labels: " + macrosStr + "\n"
        "Graph edges: " + edgesStr + "\n"
        "Main oscillators: " + osc.summary + "\n\n"
        "Audio descriptors (from rendered phrase):\n"
        + desc.describe();

    vibesynth::ChatRequest req;
    req.endpoint = cfg.endpoint;
    req.model = cfg.generateModel;
    req.temperature = 0.2; // Low temperature for consistent evaluation
    req.maxTokens = 4096;
    req.reasoningEffort = cfg.reasoningEffort;
    req.messages.push_back ({ "system", systemPrompt });
    req.messages.push_back ({ "user", userContent });

    auto sendRequest = [&] (const vibesynth::ChatRequest& r) -> vibesynth::ChatReply
    {
        if (transport)
            return transport (r);
        return vibesynth::MetaClient::send (r, apiKey, nullptr);
    };

    vibesynth::ChatReply reply = sendRequest (req);
    res.promptTokens += reply.promptTokens;
    res.completionTokens += reply.completionTokens;
    res.seconds += reply.seconds;

    int score = 0;
    std::string reason;

    if (reply.ok() && parseJudgeReply (reply.content, score, reason))
    {
        res.score = score;
        res.reason = reason;
        return res;
    }

    // Malformed reply: retry once
    vibesynth::ChatRequest retryReq = req;
    retryReq.messages.push_back ({ "assistant", reply.content });
    retryReq.messages.push_back ({ "user",
        "Your previous reply was not valid JSON or was missing score (1-5) or reason.\n"
        "Please reply with JSON only: {\"score\": 1-5, \"reason\": \"<one sentence>\"}" });

    vibesynth::ChatReply retryReply = sendRequest (retryReq);
    res.promptTokens += retryReply.promptTokens;
    res.completionTokens += retryReply.completionTokens;
    res.seconds += retryReply.seconds;

    if (retryReply.ok() && parseJudgeReply (retryReply.content, score, reason))
    {
        res.score = score;
        res.reason = reason;
    }
    else
    {
        res.score = 0;
        res.reason = "no score";
        res.malformed = true;
    }

    return res;
}

} // namespace vibesynth_cli
