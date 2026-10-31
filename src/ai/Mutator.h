#pragma once

// M11 Mutate (SPEC §9.7): the active patch sent back as 4 requests at once,
// one per direction (edit/edit_directions.md: darker, movement, brighter,
// weirder). Each slot is a full Generator run (GenerationKind::Mutate:
// parse -> lint -> compile -> offline test -> self-heal), on its own thread.
//
//   HTTP       the 4 requests run in parallel.
//   Testing    one offline test at a time (a shared validation gate):
//              concurrent tests skew the CPU measurement and can fail its
//              budgets.
//   HTTP 429   (or a reply whose message says "rate limit") is retried
//              after a back-off (Retry-After, else 2 s, 4 s, 8 s, capped);
//              from the second 429 on, every slot's requests go one at a
//              time for the rest of the run. After kRateLimitRetries
//              back-offs the slot fails with the mapped 429 message.
//              Back-offs don't use up self-heal retries.
//   Cancel     the shared flag stops every slot (requests within ~100 ms,
//              tests after their current sample, back-offs at once).
//
// Blocking: the plugin's mutate worker or vibesynth_cli; never the audio
// thread. Callbacks run on the slot threads (the caller marshals them).

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "ai/Generator.h"

namespace vibesynth {

struct MutateRequest
{
    Patch base;                     // the active patch (trim set)
    TouchedNodes baseNodes;         // probed once here if not known
    bool baseNodesKnown = false;
    AiConfig config;
    std::string apiKey;             // memory only
    double sampleRate = 48000.0;
    // Empty: the 4 embedded directions.
    std::vector<MutateDirection> directions;
    // Tests: replaces MetaClient::send (gets the run's cancel flag) and the
    // voice-cost measurement (Generator's measureVoice).
    std::function<ChatReply (const ChatRequest&, const std::atomic<bool>* cancel)> transport;
    std::function<double (const Patch&)> measureVoice;
    double backoffSeconds = 2.0;    // the first 429 wait without Retry-After
    double maxBackoffSeconds = 20.0;
};

struct MutateResult
{
    std::vector<MutateDirection> directions;  // per slot
    std::vector<GenerationResult> slots;      // per slot
    std::vector<int> rateLimited;             // 429s seen per slot
    bool wentSequential = false;              // the 429 fallback kicked in
    bool cancelled = false;
    double seconds = 0.0;                     // wall time of the whole run

    int passed() const noexcept;
    int promptTokens() const noexcept;
    int completionTokens() const noexcept;
};

// M10 merge (SPEC §9.6): the model only sees code, so Mutate first bakes
// the active patch's tweaks and macro maps into it (patch/PatchBake.h, in
// memory); the variations then carry the user's shaping in their code and
// start with no tweaks. If bake refuses, the unbaked code is sent and
// `note` says the tweaks weren't included. A patch without tweaks passes
// through unchanged. Callers put `patch` in MutateRequest::base, with
// baseNodesKnown false when `baked` (a bypassed call may have gone).
struct MutateBase
{
    Patch patch;          // what to send (never has tweaks)
    bool hadTweaks = false;
    bool baked = false;
    std::string note;     // empty unless bake refused
};
MutateBase prepareMutateBase (const Patch& active);

class Mutator
{
public:
    static constexpr int kSlots = 4;
    static constexpr int kRateLimitRetries = 3;

    static MutateResult run (const MutateRequest& request, const std::atomic<bool>* cancel,
                             const std::function<void (int slot, const GenerationProgress&)>& onProgress = {},
                             const std::function<void (int slot, const GenerationResult&)>& onDone = {});

    // A 429, or another error whose message says it was rate limited.
    static bool isRateLimit (const ChatReply& reply);
};

} // namespace vibesynth
