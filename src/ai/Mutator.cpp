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

// Shared by parallel slots: once rate limiting persists, requests go one at a
// time.
struct RateGate
{
    std::mutex serial;
    std::atomic<bool> sequential { false };
};

struct SlotConfig
{
    GenerationRequest request;
    MutateDirection direction;
};

struct ParallelRunOptions
{
    AiConfig config;
    std::string apiKey;
    double sampleRate = 48000.0;
    std::function<ChatReply (const ChatRequest&, const std::atomic<bool>* cancel)> transport;
    std::function<double (const Patch&)> measureVoice;
    double backoffSeconds = 2.0;
    double maxBackoffSeconds = 20.0;
};

MutateResult runSlots (std::vector<SlotConfig> slots,
                       const ParallelRunOptions& options,
                       const std::atomic<bool>* cancel,
                       const std::function<void (int, const GenerationProgress&)>& onProgress,
                       const std::function<void (int, const GenerationResult&)>& onDone)
{
    const auto t0 = Clock::now();
    MutateResult out;
    const int n = (int) slots.size();
    out.slots.resize ((size_t) n);
    out.directions.resize ((size_t) n);
    out.rateLimited.assign ((size_t) n, 0);
    for (int i = 0; i < n; ++i)
        out.directions[(size_t) i] = slots[(size_t) i].direction;

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
                    if (options.transport)
                        return options.transport (c, cancel);
                    return MetaClient::send (c, options.apiKey, cancel, [&] (double t)
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
                    if (! Mutator::isRateLimit (r) || hit >= Mutator::kRateLimitRetries)
                        return r;
                    ++out.rateLimited[(size_t) slot];
                    // The first 429 waits and retries in parallel; a second
                    // one means the service wants fewer requests at once.
                    if (hit >= 1)
                        rate.sequential = true;
                    const double wait = r.retryAfterSeconds > 0
                                            ? (double) r.retryAfterSeconds
                                            : options.backoffSeconds * std::pow (2.0, (double) hit);
                    if (! sleepUnlessCancelled (std::min (wait, options.maxBackoffSeconds), cancel))
                    {
                        ChatReply c2;
                        c2.error = ChatError::Cancelled;
                        c2.message = "cancelled";
                        return c2;
                    }
                }
            };

            auto gr = slots[(size_t) slot].request;
            gr.config = options.config;
            gr.apiKey = options.apiKey;
            gr.sampleRate = options.sampleRate;
            gr.measureVoice = options.measureVoice;
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

} // namespace

MutateBase prepareMutateBase (const Patch& active)
{
    MutateBase b;
    b.patch = active;
    b.hadTweaks = ! active.tweaks.empty() || ! active.macroMaps.empty() || ! active.macroRanges.empty();
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
    b.patch.macroRanges.clear();
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
    std::vector<MutateDirection> dirs;
    if (request.directions.empty())
    {
        // Old behaviour (SPEC §9.7, M14b): keep the first 4 directions
        const auto& all = PromptResources::embedded().mutateDirections;
        for (size_t i = 0; i < std::min<size_t> (4, all.size()); ++i)
            dirs.push_back (all[i]);
    }
    else
    {
        dirs = request.directions;
    }
    if ((int) dirs.size() > kSlots)
        dirs.resize ((size_t) kSlots);
    const int n = (int) dirs.size();

    // The base's node list once, not once per slot.
    TouchedNodes baseNodes = request.baseNodes;
    if (! request.baseNodesKnown)
        baseNodes = probeTouchedNodes (request.base, request.sampleRate, cancel);

    std::vector<SlotConfig> slots;
    slots.reserve ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        SlotConfig sc;
        sc.direction = dirs[(size_t) i];
        sc.request.kind = GenerationKind::Mutate;
        sc.request.base = request.base;
        sc.request.baseNodes = baseNodes;
        sc.request.baseNodesKnown = true;
        sc.request.direction = dirs[(size_t) i];
        slots.push_back (std::move (sc));
    }

    ParallelRunOptions opts;
    opts.config = request.config;
    opts.apiKey = request.apiKey;
    opts.sampleRate = request.sampleRate;
    opts.transport = request.transport;
    opts.measureVoice = request.measureVoice;
    opts.backoffSeconds = request.backoffSeconds;
    opts.maxBackoffSeconds = request.maxBackoffSeconds;

    return runSlots (std::move (slots), opts, cancel, onProgress, onDone);
}

MutateResult Mutator::runTakes (const TakesRequest& request, const std::atomic<bool>* cancel,
                                const std::function<void (int, const GenerationProgress&)>& onProgress,
                                const std::function<void (int, const GenerationResult&)>& onDone)
{
    const int count = std::clamp (request.takes, 1, 4);
    const auto dirs = buildTakeDirections (count);
    const int n = (int) dirs.size();

    std::vector<SlotConfig> slots;
    slots.reserve ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        SlotConfig sc;
        sc.direction = dirs[(size_t) i];
        sc.request.kind = GenerationKind::Generate;
        sc.request.prompt = request.prompt;
        sc.request.extraUserLine = dirs[(size_t) i].text;
        slots.push_back (std::move (sc));
    }

    ParallelRunOptions opts;
    opts.config = request.config;
    opts.apiKey = request.apiKey;
    opts.sampleRate = request.sampleRate;
    opts.transport = request.transport;
    opts.measureVoice = request.measureVoice;
    opts.backoffSeconds = request.backoffSeconds;
    opts.maxBackoffSeconds = request.maxBackoffSeconds;

    return runSlots (std::move (slots), opts, cancel, onProgress, onDone);
}

std::vector<MutateDirection> Mutator::buildTakeDirections (int count)
{
    count = std::clamp (count, 1, 4);
    const std::string nStr = std::to_string (count);
    static const struct {
        const char* key;
        const char* label;
        const char* templateText;
    } kTakes[4] = {
        { "literal", "Literal", "Take 1 of %s: the most literal reading of the request." },
        { "alt_source", "Alt source", "Take 2 of %s: the same intent built on a different main sound source than the obvious one (square/pulse, FM, wavetable, sync, noise, or a physical model)." },
        { "alt_motion", "Alt motion", "Take 3 of %s: the same intent with a different kind of movement or rhythm." },
        { "bold", "Bold", "Take 4 of %s: a bolder, more extreme reading, still mix-ready." }
    };

    std::vector<MutateDirection> dirs;
    for (int i = 0; i < count; ++i)
    {
        MutateDirection d;
        d.key = kTakes[i].key;
        d.label = kTakes[i].label;
        char buf[256];
        std::snprintf (buf, sizeof (buf), kTakes[i].templateText, nStr.c_str());
        d.text = buf;
        dirs.push_back (d);
    }
    return dirs;
}

std::vector<MutateDirection> Mutator::buildSteeredDirections (const std::string& rawText, int count)
{
    const std::string text = rawText;
    count = std::clamp (count, 1, kSlots);
    struct Intensity
    {
        const char* word;
        const char* phrase;
    };
    static constexpr Intensity kLevels[5] = {
        { "subtle", "subtle: a small, careful step" },
        { "light", "light: a gentle step" },
        { "moderate", "moderate: a balanced step" },
        { "strong", "strong: a distinct push" },
        { "bold", "bold: push it much further" }
    };

    // Card label: first ~18 chars + intensity word (e.g. "more metallic… · bold")
    std::string shortText = text;
    if (shortText.length() > 18)
    {
        const size_t comma = shortText.find (',');
        if (comma != std::string::npos && comma >= 8 && comma <= 18)
            shortText = shortText.substr (0, comma);
        else
        {
            shortText = shortText.substr (0, 18);
            // don't end inside a multi-byte UTF-8 character
            while (! shortText.empty() && (static_cast<unsigned char> (shortText.back()) & 0xC0) == 0x80)
                shortText.pop_back();
            if (! shortText.empty() && (static_cast<unsigned char> (shortText.back()) & 0x80) != 0)
                shortText.pop_back();
        }
        while (! shortText.empty() && (shortText.back() == ' ' || shortText.back() == ','))
            shortText.pop_back();
        shortText += "…";
    }

    std::vector<MutateDirection> dirs;
    dirs.reserve ((size_t) count);
    for (int k = 0; k < count; ++k)
    {
        int levelIdx = 2; // moderate when count == 1
        if (count > 1)
            levelIdx = (int) std::round (k * 4.0 / (count - 1));
        const auto& lvl = kLevels[levelIdx];

        MutateDirection d;
        d.key = "steer_" + std::to_string (k + 1);
        d.label = shortText + " \xc2\xb7 " + lvl.word;
        d.text = text + "\nIntensity: " + lvl.phrase + " (take " + std::to_string (k + 1)
               + " of " + std::to_string (count) + ")";
        dirs.push_back (std::move (d));
    }
    return dirs;
}

std::vector<MutateDirection> Mutator::buildAutoDirections (const std::vector<std::string>& chipKeys)
{
    const auto& all = PromptResources::embedded().mutateDirections;
    std::vector<MutateDirection> dirs;
    for (const auto& key : chipKeys)
    {
        for (const auto& d : all)
        {
            if (d.key == key)
            {
                dirs.push_back (d);
                break;
            }
        }
    }
    return dirs;
}

} // namespace vibesynth
