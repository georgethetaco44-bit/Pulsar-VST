// The generation pipeline (SPEC §6.4). See Generator.h.

#include "ai/Generator.h"

#include <chrono>
#include <cmath>

#include "ai/MetaClient.h"
#include "ai/PromptBuilder.h"
#include "ai/ResponseParser.h"
#include "patch/PatchValidator.h"

namespace vibesynth {

namespace {

// Validator status -> pipeline stage (the --batch histogram).
const char* stageOf (PatchStatus s) noexcept
{
    switch (s)
    {
        case PatchStatus::LintError:    return "lint";
        case PatchStatus::NestedLoops:
        case PatchStatus::CompileError:
        case PatchStatus::TooLarge:     return "compile";
        case PatchStatus::Ok:
        case PatchStatus::BadJson:
        case PatchStatus::BadField:
        case PatchStatus::MissingVoice:
        case PatchStatus::NonFinite:
        case PatchStatus::TooLoud:
        case PatchStatus::DcOffset:
        case PatchStatus::Silent:
        case PatchStatus::TooSlow:      break;
    }
    return "test";
}

} // namespace

ChatReply testApiKey (const AiConfig& config, const std::string& apiKey, const std::atomic<bool>* cancel)
{
    ChatRequest request;
    request.endpoint = config.endpoint;
    request.model = config.generateModel;
    request.temperature = config.retryTemperature;
    request.maxTokens = 16;
    request.reasoningEffort = config.reasoningEffort;
    request.messages = { { "user", "Reply with: ok" } };
    return MetaClient::send (request, apiKey, cancel);
}

bool keyTestPassed (const ChatReply& reply) noexcept
{
    // finishReason is only set once a choice was parsed, so a 200 that is
    // not the chat-completions JSON (a captive portal, a wrong endpoint)
    // still fails.
    return reply.ok() || (reply.error == ChatError::BadReply && reply.httpStatus == 200 && ! reply.finishReason.empty());
}

std::string keyTestMessage (const ChatReply& reply)
{
    if (reply.error == ChatError::Auth)
        return "Key rejected (HTTP " + std::to_string (reply.httpStatus) + "). Check that it was copied in full.";
    return keyTestPassed (reply) ? "Key works" : reply.message;
}

std::string describeProgress (const GenerationProgress& p)
{
    std::string s;
    switch (p.stage)
    {
        case GenerationStage::Building:  s = "Building prompt"; break;
        case GenerationStage::Waiting:
            s = "Waiting for model (" + std::to_string (static_cast<int> (p.waitSeconds)) + " s)";
            break;
        case GenerationStage::Parsing:   s = "Parsing reply"; break;
        case GenerationStage::Testing:   s = "Testing patch"; break;
        case GenerationStage::Done:      return "Done";
        case GenerationStage::Failed:    return "Failed";
        case GenerationStage::Cancelled: return "Cancelled";
    }
    if (p.retry > 0)
        s += " (retry " + std::to_string (p.retry) + "/" + std::to_string (Generator::kMaxRetries) + ")";
    return s;
}

int GenerationResult::promptTokens() const noexcept
{
    int n = 0;
    for (const auto& a : attempts)
        n += a.promptTokens;
    return n;
}

int GenerationResult::completionTokens() const noexcept
{
    int n = 0;
    for (const auto& a : attempts)
        n += a.completionTokens;
    return n;
}

GenerationResult Generator::run (const GenerationRequest& request, const std::atomic<bool>* cancel,
                                 const std::function<void (const GenerationProgress&)>& onProgress)
{
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    GenerationResult res;
    auto progress = [&onProgress] (GenerationStage stage, int retry, double wait = 0.0)
    {
        if (onProgress)
            onProgress ({ stage, retry, wait });
    };
    auto cancelled = [cancel] { return cancel != nullptr && cancel->load(); };
    auto finish = [&] (GenerationResult& r) -> GenerationResult&
    {
        r.seconds = std::chrono::duration<double> (Clock::now() - t0).count();
        progress (r.cancelled ? GenerationStage::Cancelled
                              : r.ok ? GenerationStage::Done : GenerationStage::Failed, 0);
        return r;
    };

    const PromptBuilder builder;
    RetryContext retry;
    bool haveRetry = false;
    bool weightHintUsed = false;
    bool haveHeavy = false;
    Patch heavy;
    double heavyNs = 0.0;
    TouchedNodes heavyTouched;

    for (int n = 0; n <= kMaxRetries; ++n)
    {
        if (cancelled())
        {
            res.cancelled = true;
            res.reason = "cancelled";
            return finish (res);
        }
        progress (GenerationStage::Building, n);
        const BuiltPrompt bp = builder.build (request.prompt, haveRetry ? &retry : nullptr);
        ChatRequest cr;
        cr.endpoint = request.config.endpoint;
        cr.model = n == 0 ? request.config.generateModel : request.config.retryModel;
        cr.temperature = n == 0 ? request.config.generateTemperature : request.config.retryTemperature;
        cr.reasoningEffort = request.config.reasoningEffort;
        cr.messages = bp.messages;

        GenerationAttempt a;
        a.recipes = bp.recipeFiles;
        a.examples = bp.exampleFiles;
        a.temperature = cr.temperature;
        progress (GenerationStage::Waiting, n, 0.0);
        const ChatReply reply = request.transport
                                    ? request.transport (cr)
                                    : MetaClient::send (cr, request.apiKey, cancel,
                                                        [&] (double t) { progress (GenerationStage::Waiting, n, t); });
        a.promptTokens = reply.promptTokens;
        a.completionTokens = reply.completionTokens;
        a.reasoningTokens = reply.reasoningTokens;
        a.requestSeconds = reply.seconds;
        if (! reply.ok())
        {
            a.stage = "request";
            a.status = chatErrorName (reply.error);
            a.reason = reply.message;
            res.attempts.push_back (a);
            res.reason = reply.message;
            if (reply.error == ChatError::Cancelled)
            {
                res.cancelled = true;
                return finish (res);
            }
            // An empty, garbled or slow reply may come out fine next time
            // (same retry context); key, quota, network and settings
            // errors can't be fixed by asking again.
            if (reply.error == ChatError::BadReply || reply.error == ChatError::Timeout)
                continue;
            break;
        }
        a.reply = reply.content;

        progress (GenerationStage::Parsing, n);
        const ResponseParseResult pr = parseResponse (reply.content);
        a.notes = pr.notes;
        if (! pr.ok())
        {
            a.stage = "parse";
            a.status = patchStatusName (pr.status);
            a.reason = pr.error;
            res.attempts.push_back (a);
            res.reason = pr.error;
            retry = { reply.content, pr.error };
            haveRetry = true;
            continue;
        }

        Patch patch = pr.patch;
        patch.prompt = request.prompt;
        progress (GenerationStage::Testing, n);
        const ValidationResult vr = validatePatch (patch, request.sampleRate, cancel);
        if (vr.cancelled)
        {
            res.cancelled = true;
            res.reason = "cancelled";
            return finish (res);
        }
        if (! vr.ok())
        {
            a.stage = stageOf (vr.status);
            a.status = patchStatusName (vr.status);
            a.reason = vr.error;
            res.attempts.push_back (a);
            res.reason = vr.error;
            retry = { reply.content, vr.error };
            haveRetry = true;
            continue;
        }
        patch.trim = vr.trim;
        const double voiceNs = request.measureVoice ? request.measureVoice (patch)
                                                    : measureVoiceNs (patch, request.sampleRate);
        a.voiceNs = voiceNs;
        const double budget = patch.voiceBudgetNs();

        if (voiceNs > kWeightHintFactor * budget * validatorCpuScale() && ! weightHintUsed && n < kMaxRetries)
        {
            a.stage = "weight";
            a.status = "heavy";
            a.reason = "too heavy: " + std::to_string (static_cast<int> (std::lround (voiceNs)))
                     + " ns per voice-sample, budget " + std::to_string (static_cast<int> (budget)) + " ("
                     + patch.voiceMode + "); simplify";
            res.attempts.push_back (a);
            weightHintUsed = true;
            haveHeavy = true;
            heavy = patch;
            heavyNs = voiceNs;
            heavyTouched = vr.touched;
            retry = { reply.content, a.reason };
            haveRetry = true;
            continue;
        }

        a.stage = "ok";
        a.status = "ok";
        res.attempts.push_back (a);
        res.ok = true;
        res.patch = patch;
        res.voiceNs = voiceNs;
        res.voiceBudgetNs = budget;
        res.touched = vr.touched;
        res.reason.clear();
        return finish (res);
    }

    if (haveHeavy)
    {
        // The lighter retry failed: keep the heavy patch that passed.
        res.ok = true;
        res.patch = heavy;
        res.voiceNs = heavyNs;
        res.voiceBudgetNs = heavy.voiceBudgetNs();
        res.touched = heavyTouched;
        res.reason = "kept the heavier version (the lighter retry failed: " + res.reason + ")";
    }
    return finish (res);
}

} // namespace vibesynth
