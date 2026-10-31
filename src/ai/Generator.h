#pragma once

// The generation pipeline (SPEC §3 steps 1-5, §6.4), shared by the plugin's
// generation worker and vibesynth_cli --batch:
//
//   build prompt -> request -> parse -> lint -> compile -> offline test
//
// A parse, lint, compile or test failure retries (at most kMaxRetries) at
// the retry temperature with the model's own reply and the exact reason.
// Weight hint: a patch that passes but whose measured voice cost is over
// 1.5x its budget (poly 300 / mono 1,200 ns) gets one "too heavy" retry if
// one is left; if that retry fails, the heavy patch is still accepted
// (CPU is never a hard reject beyond the §7.1 runaway rule). The budget is
// multiplied by validatorCpuScale() (vibesynth_cli under Rosetta). An empty,
// garbled or timed-out reply uses up an attempt and is asked again; other
// request errors (no key, auth, rate limit, network, server, bad model)
// end the run at once: retrying can't fix them.
//
// Blocking; runs on a worker thread or in the CLI, never the audio
// thread. `cancel` is polled by the request (every 100 ms) and the
// validator (every sample), so a cancelled run returns within moments.

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "ai/AiSettings.h"
#include "ai/MetaClient.h"
#include "ai/PromptBuilder.h"
#include "engine/NodeMeters.h"
#include "patch/Patch.h"

namespace vibesynth {

// M11 (SPEC §9.7): Generate writes a new patch from `prompt`; Change and
// Mutate edit `base` and run the same parse -> lint -> compile -> test ->
// self-heal path. Change sends the node id and the instruction (the node
// must be one `base` really touches, else nothing is sent); Mutate sends a
// direction and uses the mutate model and temperature. `prompt` may be
// left empty for an edit: the result's prompt is then the base prompt plus
// a line saying what was changed.
enum class GenerationKind { Generate, Change, Mutate };

struct GenerationRequest
{
    GenerationKind kind = GenerationKind::Generate;
    std::string prompt;
    Patch base;                     // Change, Mutate: the active patch (trim set)
    TouchedNodes baseNodes;         // its node list, if known (else probed here)
    bool baseNodesKnown = false;
    std::string node;               // Change: "flt_lp:0", "master/bus_sat#1", "fx_sat #1"
    std::string instruction;        // Change: what to do
    MutateDirection direction;      // Mutate
    // Parallel runs (Mutate) share one gate so only one offline test runs
    // at a time: concurrent tests skew the CPU measurement (SPEC §7.1).
    std::mutex* validationGate = nullptr;
    AiConfig config;
    std::string apiKey;           // held in memory only
    double sampleRate = 48000.0;  // the validator's rate
    // Replaces MetaClient::send (the smoketest's offline self-heal cases).
    std::function<ChatReply (const ChatRequest&)> transport;
    // Replaces measureVoiceNs (the smoketest's weight-hint cases), so the
    // hint fires on the same patches on any machine.
    std::function<double (const Patch&)> measureVoice;
};

enum class GenerationStage
{
    Building,   // building the prompt
    Waiting,    // request sent, waiting for the model
    Parsing,
    Testing,    // lint + compile + offline matrix
    Done,
    Failed,
    Cancelled
};

struct GenerationProgress
{
    GenerationStage stage = GenerationStage::Building;
    int retry = 0;           // 0 = first attempt, then 1..kMaxRetries
    double waitSeconds = 0;  // Waiting only
};

// "Waiting for model (4 s)", "Testing (retry 1/2)", ...
std::string describeProgress (const GenerationProgress& p);

struct GenerationAttempt
{
    std::string reply;          // the model's raw output (empty on a request error)
    // Where it ended: "request", "parse", "lint", "compile", "test",
    // "weight" (passed but too heavy) or "ok".
    std::string stage;
    std::string status;         // patchStatusName / chatErrorName detail ("nan", "timeout", ...)
    std::string reason;         // the exact reason (sent back on the next retry)
    std::vector<std::string> notes;  // parser fix-ups
    std::vector<std::string> recipes;
    std::vector<std::string> examples;
    int promptTokens = 0;
    int completionTokens = 0;   // includes reasoningTokens
    int reasoningTokens = 0;
    double requestSeconds = 0.0;
    double voiceNs = 0.0;       // measured, when the patch passed the test
    double temperature = 0.0;
};

struct GenerationResult
{
    bool ok = false;
    bool cancelled = false;
    Patch patch;                // ok: validated, trim set, prompt = the user's prompt
    std::string reason;         // the last failure reason (or a note when ok)
    std::vector<GenerationAttempt> attempts;
    double voiceNs = 0.0;       // the accepted patch's measured voice cost
    double voiceBudgetNs = 0.0;
    double seconds = 0.0;
    TouchedNodes touched;       // ok: the offline test's node list (M9, SPEC §9.6)
    // M11 edits, when ok: base nodes the new patch no longer has ("" = all
    // kept), and base macro labels it changed ("m2 Cutoff -> Wobble").
    std::string lostNodes;
    std::vector<std::string> labelChanges;

    int promptTokens() const noexcept;
    int completionTokens() const noexcept;
};

// Settings "Test key" (M9): one tiny request with the generate model, so a
// bad key, endpoint or model ID shows up before the first generation. Blocks
// like Generator::run (worker thread or CLI); `cancel` is polled the same way.
ChatReply testApiKey (const AiConfig& config, const std::string& apiKey, const std::atomic<bool>* cancel);

// The key works: the service answered, or answered with a well-formed 200
// whose message is empty (a reasoning model spending the 16-token limit
// thinking still proves the key, endpoint and model). Everything else keeps
// its mapped message.
bool keyTestPassed (const ChatReply& reply) noexcept;
// What Settings shows under the key field. Like the reply's message, but a
// rejected key doesn't send the user to Settings (they are in it).
std::string keyTestMessage (const ChatReply& reply);

class Generator
{
public:
    static constexpr int kMaxRetries = 2;          // SPEC §6.4
    static constexpr double kWeightHintFactor = 1.5;

    static GenerationResult run (const GenerationRequest& request, const std::atomic<bool>* cancel,
                                 const std::function<void (const GenerationProgress&)>& onProgress = {});
};

} // namespace vibesynth
