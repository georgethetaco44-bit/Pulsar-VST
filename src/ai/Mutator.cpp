// M11 Mutate. See Mutator.h.

#include "ai/Mutator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

#include "patch/PatchBake.h"
#include "patch/PatchTweaks.h"
#include "patch/PatchValidator.h"

namespace vibesynth {

namespace {

using Clock = std::chrono::steady_clock;

std::string lowerCase (std::string s)
{
    for (auto& c : s)
        c = (c >= 'A' && c <= 'Z') ? static_cast<char> (c - 'A' + 'a') : c;
    return s;
}

// Sleeps up to `seconds`, waking every 20 ms to check `cancel`. False if
// cancelled.
bool sleepUnlessCancelled (double seconds, const std::atomic<bool>* cancel)
{
    const auto until = Clock::now() + std::chrono::microseconds ((long long) (seconds * 1.0e6));
    while (Clock::now() < until)
    {
        if (cancel != nullptr && cancel->load())
            return false;
        std::this_thread::sleep_for (std::chrono::milliseconds (std::min<long long> (
            20, std::chrono::duration_cast<std::chrono::milliseconds> (until - Clock::now()).count() + 1)));
    }
    return cancel == nullptr || ! cancel->load();
}

// Shared by the 4 slots: once rate limiting persists, requests go one at a
// time.
struct RateGate
{
    std::mutex serial;
    std::atomic<bool> sequential { false };
};

} // namespace

MutateBase prepareMutateBase (const Patch& active)
{
    MutateBase b;
    b.patch = active;
    b.hadTweaks = ! active.tweaks.empty() || ! active.macroMaps.empty();
    if (! b.hadTweaks)
    {
        carryMutateMods (active, b.patch);
        return b;
    }
    const BakeResult baked = bakePatch (active);
    if (baked.ok)
    {
        b.patch = baked.patch;
        b.baked = true;
    }
    else
    {
        b.note = "Your tweaks weren't included in the variations (they couldn't be written into the code: "
               + baked.error + ").";
    }
    b.patch.tweaks.clear();
    b.patch.macroMaps.clear();
    carryMutateMods (active, b.patch);
    return b;
}

bool Mutator::isRateLimit (const ChatReply& reply)
{
    if (reply.error == ChatError::RateLimited)
        return true;
    if (reply.ok() || reply.error == ChatError::Cancelled)
        return false;
    const std::string m = lowerCase (reply.message);
    return m.find ("rate limit") != std::string::npos || m.find ("rate-limit") != std::string::npos
        || m.find ("too many requests") != std::string::npos;
}

int MutateResult::passed() const noexcept
{
    int n = 0;
    for (const auto& s : slots)
        n += s.ok ? 1 : 0;
    return n;
}

int MutateResult::promptTokens() const noexcept
{
    int n = 0;
    for (const auto& s : slots)
        n += s.promptTokens();
    return n;
}

int MutateResult::completionTokens() const noexcept
{
    int n = 0;
    for (const auto& s : slots)
        n += s.completionTokens();
    return n;
}

MutateResult Mutator::run (const MutateRequest& request, const std::atomic<bool>* cancel,
                           const std::function<void (int, const GenerationProgress&)>& onProgress,
                           const std::function<void (int, const GenerationResult&)>& onDone)
{
    const auto t0 = Clock::now();
    MutateResult out;
    out.directions = request.directions.empty() ? PromptResources::embedded().mutateDirections : request.directions;
    if ((int) out.directions.size() > kSlots)
        out.directions.resize ((size_t) kSlots);
    const int n = (int) out.directions.size();
    out.slots.resize ((size_t) n);
    out.rateLimited.assign ((size_t) n, 0);

    // The base's node list once, not once per slot.
    TouchedNodes baseNodes = request.baseNodes;
    if (! request.baseNodesKnown)
        baseNodes = probeTouchedNodes (request.base, request.sampleRate, cancel);

    std::mutex validationGate;
    RateGate rate;
    std::vector<std::thread> threads;
    for (int slot = 0; slot < n; ++slot)
    {
        threads.emplace_back ([&, slot]
        {
            int lastRetry = 0;
            auto send = [&] (const ChatRequest& c) -> ChatReply
            {
                auto once = [&]
                {
                    if (request.transport)
                        return request.transport (c, cancel);
                    return MetaClient::send (c, request.apiKey, cancel, [&] (double t)
                    {
                        if (onProgress)
                            onProgress (slot, { GenerationStage::Waiting, lastRetry, t });
                    });
                };
                for (int hit = 0;; ++hit)
                {
                    ChatReply r;
                    if (rate.sequential.load())
                    {
                        std::lock_guard<std::mutex> lock (rate.serial);
                        r = once();
                    }
                    else
                    {
                        r = once();
                    }
                    if (! isRateLimit (r) || hit >= kRateLimitRetries)
                        return r;
                    ++out.rateLimited[(size_t) slot];
                    // The first 429 waits and retries in parallel; a second
                    // one means the service wants fewer requests at once.
                    if (hit >= 1)
                        rate.sequential = true;
                    const double wait = r.retryAfterSeconds > 0
                                            ? (double) r.retryAfterSeconds
                                            : request.backoffSeconds * std::pow (2.0, (double) hit);
                    if (! sleepUnlessCancelled (std::min (wait, request.maxBackoffSeconds), cancel))
                    {
                        ChatReply c2;
                        c2.error = ChatError::Cancelled;
                        c2.message = "cancelled";
                        return c2;
                    }
                }
            };

            GenerationRequest gr;
            gr.kind = GenerationKind::Mutate;
            gr.base = request.base;
            gr.baseNodes = baseNodes;
            gr.baseNodesKnown = true;
            gr.direction = out.directions[(size_t) slot];
            gr.config = request.config;
            gr.apiKey = request.apiKey;
            gr.sampleRate = request.sampleRate;
            gr.measureVoice = request.measureVoice;
            gr.validationGate = &validationGate;
            gr.transport = send;
            out.slots[(size_t) slot] = Generator::run (gr, cancel, [&] (const GenerationProgress& p)
            {
                lastRetry = p.retry;
                if (onProgress)
                    onProgress (slot, p);
            });
            if (onDone)
                onDone (slot, out.slots[(size_t) slot]);
        });
    }
    for (auto& t : threads)
        t.join();

    out.wentSequential = rate.sequential.load();
    out.cancelled = cancel != nullptr && cancel->load();
    out.seconds = std::chrono::duration<double> (Clock::now() - t0).count();
    return out;
}

} // namespace vibesynth
