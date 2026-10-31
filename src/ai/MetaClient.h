#pragma once

// Chat-completions client (SPEC §6.3, §6.6): one OpenAI-compatible POST
// through juce::WebInputStream (WinHTTP, NSURLSession, or libcurl on Linux
// with JUCE_USE_CURL=1).
//
// send() blocks its caller (the generation worker or vibesynth_cli) and
// runs the request on a helper thread it always joins before returning.
// While it waits it polls `cancel` and the 30 s deadline every 100 ms and,
// on either, cancels the stream, so a processor destructor that cancels
// and joins the worker never waits more than a moment.
//
// Errors come back as short user messages (no key, 401/403, 429 with
// Retry-After, 5xx, timeout, network down, empty or garbled body). Nothing
// here logs; the key, the Authorization header and request bodies never
// appear in any message. Never called from a constructor or
// prepareToPlay, so the plugin makes no network calls under pluginval.

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace vibesynth {

struct ChatMessage
{
    std::string role;    // "system", "user" or "assistant"
    std::string content;
};

struct ChatRequest
{
    std::string endpoint;
    std::string model;
    double temperature = 0.8;
    int maxTokens = 8192;              // includes hidden reasoning tokens
    std::string reasoningEffort;       // "low", "minimal", ...; empty = not sent
    std::vector<ChatMessage> messages;
};

enum class ChatError
{
    None,
    NoKey,
    Auth,         // 401/403
    RateLimited,  // 429
    Server,       // 5xx
    Rejected,     // other 4xx (bad model ID, bad request)
    Timeout,
    Network,      // no connection
    BadReply,     // empty or garbled body
    Cancelled
};

const char* chatErrorName (ChatError e) noexcept;

struct ChatReply
{
    ChatError error = ChatError::None;
    std::string message;        // short user-readable reason when error != None
    std::string content;        // choices[0].message.content
    int httpStatus = 0;
    int promptTokens = 0;       // from "usage" (0 if the reply had none)
    int completionTokens = 0;   // includes reasoningTokens
    int reasoningTokens = 0;    // usage.completion_tokens_details.reasoning_tokens
    std::string finishReason;
    int retryAfterSeconds = 0;  // 429 only, from Retry-After
    double seconds = 0.0;

    bool ok() const noexcept { return error == ChatError::None; }
};

class MetaClient
{
public:
    static constexpr int kTimeoutMs = 30000;  // SPEC §6.6

    // `onWaiting` (optional) gets the elapsed seconds about every 100 ms,
    // on the calling thread.
    static ChatReply send (const ChatRequest& request, const std::string& apiKey,
                           const std::atomic<bool>* cancel,
                           const std::function<void (double)>& onWaiting = {});

    // The JSON body for `request` (exposed for tests; holds no key).
    static std::string requestBody (const ChatRequest& request);
    // Maps an HTTP status + body to a reply (exposed for tests). `apiKey`
    // is only used to scrub it from any server message.
    static ChatReply interpret (int status, const std::string& body, const std::string& retryAfter,
                                const std::string& apiKey);
};

} // namespace vibesynth
