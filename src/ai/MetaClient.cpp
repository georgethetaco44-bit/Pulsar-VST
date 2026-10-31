// Chat-completions client (SPEC §6.6). See MetaClient.h.

#include "ai/MetaClient.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace vibesynth {

namespace {

ChatReply failure (ChatError e, std::string message, int status = 0)
{
    ChatReply r;
    r.error = e;
    r.message = std::move (message);
    r.httpStatus = status;
    return r;
}

// One line, at most 160 characters, with the key (if a server echoes it)
// replaced.
std::string shortServerText (juce::String text, const std::string& apiKey)
{
    if (apiKey.size() >= 8)
        text = text.replace (juce::String (apiKey), "***");
    text = text.replaceCharacters ("\r\n\t", "   ").trim();
    if (text.length() > 160)
        text = text.substring (0, 157) + "...";
    return text.toStdString();
}

// {"error": {"message": "..."}} or {"error": "..."} or {"message": "..."}.
juce::String serverErrorText (const juce::var& root)
{
    const juce::var err = root.getProperty ("error", juce::var());
    if (err.isString())
        return err.toString();
    if (err.isObject())
        return err.getProperty ("message", "").toString();
    return root.getProperty ("message", "").toString();
}

} // namespace

const char* chatErrorName (ChatError e) noexcept
{
    switch (e)
    {
        case ChatError::None:        return "ok";
        case ChatError::NoKey:       return "no_key";
        case ChatError::Auth:        return "auth";
        case ChatError::RateLimited: return "rate_limited";
        case ChatError::Server:      return "server";
        case ChatError::Rejected:    return "rejected";
        case ChatError::Timeout:     return "timeout";
        case ChatError::Network:     return "network";
        case ChatError::BadReply:    return "bad_reply";
        case ChatError::Cancelled:   return "cancelled";
    }
    return "unknown";
}

std::string MetaClient::requestBody (const ChatRequest& request)
{
    juce::Array<juce::var> messages;
    for (const ChatMessage& m : request.messages)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("role", juce::String (m.role));
        o->setProperty ("content", juce::String::fromUTF8 (m.content.c_str()));
        messages.add (juce::var (o));
    }
    auto* root = new juce::DynamicObject();
    const juce::var rootVar (root);
    root->setProperty ("model", juce::String (request.model));
    root->setProperty ("temperature", request.temperature);
    root->setProperty ("max_tokens", request.maxTokens);
    if (! request.reasoningEffort.empty())
        root->setProperty ("reasoning_effort", juce::String (request.reasoningEffort));
    root->setProperty ("messages", messages);
    return juce::JSON::toString (rootVar, juce::JSON::FormatOptions{}
                                              .withSpacing (juce::JSON::Spacing::none)
                                              .withMaxDecimalPlaces (3))
        .toStdString();
}

ChatReply MetaClient::interpret (int status, const std::string& body, const std::string& retryAfter,
                                 const std::string& apiKey)
{
    juce::var root;
    const bool json = juce::JSON::parse (juce::String::fromUTF8 (body.data(), (int) body.size()), root).wasOk()
                      && root.isObject();

    if (status == 401 || status == 403)
        // No server text: some services echo part of the key here.
        return failure (ChatError::Auth, "API key rejected (HTTP " + std::to_string (status)
                                             + "): check it in Settings", status);
    if (status == 429)
    {
        const int wait = juce::String (retryAfter).trim().getIntValue();
        ChatReply r = failure (ChatError::RateLimited,
                               wait > 0 ? "Rate limited (HTTP 429): try again in " + std::to_string (wait) + " s"
                                        : std::string ("Rate limited (HTTP 429): try again shortly"),
                               status);
        r.retryAfterSeconds = wait;
        return r;
    }
    if (status >= 500)
        return failure (ChatError::Server, "Server error (HTTP " + std::to_string (status) + "): try again later",
                        status);
    if (status != 200)
    {
        const auto text = json ? shortServerText (serverErrorText (root), apiKey) : std::string();
        return failure (ChatError::Rejected, "Request rejected (HTTP " + std::to_string (status) + ")"
                                                 + (text.empty() ? std::string() : ": " + text),
                        status);
    }

    if (body.empty())
        return failure (ChatError::BadReply, "Empty reply from the model service", status);
    if (! json)
        return failure (ChatError::BadReply, "Garbled reply from the model service (not JSON)", status);

    ChatReply r;
    r.httpStatus = status;
    const juce::var usage = root.getProperty ("usage", juce::var());
    if (usage.isObject())
    {
        r.promptTokens = static_cast<int> (usage.getProperty ("prompt_tokens", 0));
        r.completionTokens = static_cast<int> (usage.getProperty ("completion_tokens", 0));
        r.reasoningTokens = static_cast<int> (usage.getProperty ("completion_tokens_details", juce::var())
                                                  .getProperty ("reasoning_tokens", 0));
    }
    const auto* choices = root.getProperty ("choices", juce::var()).getArray();
    if (choices == nullptr || choices->isEmpty())
        return failure (ChatError::BadReply, "Garbled reply from the model service (no choices)", status);
    const juce::var& choice = choices->getReference (0);
    r.finishReason = choice.getProperty ("finish_reason", "").toString().toStdString();
    const juce::var content = choice.getProperty ("message", juce::var()).getProperty ("content", juce::var());
    if (! content.isString() || content.toString().trim().isEmpty())
    {
        // A reasoning model can spend the whole token limit thinking.
        ChatReply e = failure (ChatError::BadReply,
                               r.finishReason == "length"
                                   ? "Empty reply: the model hit the token limit while reasoning ("
                                         + std::to_string (r.completionTokens) + " tokens, "
                                         + std::to_string (r.reasoningTokens) + " reasoning)"
                                   : std::string ("Empty reply from the model"),
                               status);
        e.promptTokens = r.promptTokens;
        e.completionTokens = r.completionTokens;
        e.reasoningTokens = r.reasoningTokens;
        e.finishReason = r.finishReason;
        return e;
    }
    r.content = content.toString().toStdString();
    return r;
}

ChatReply MetaClient::send (const ChatRequest& request, const std::string& apiKey,
                            const std::atomic<bool>* cancel,
                            const std::function<void (double)>& onWaiting)
{
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    auto elapsed = [t0] { return std::chrono::duration<double> (Clock::now() - t0).count(); };

    if (juce::String (apiKey).trim().isEmpty())
        return failure (ChatError::NoKey, "No API key: add one in Settings (or set VIBESYNTH_API_KEY)");
    const juce::URL url = juce::URL (juce::String (request.endpoint))
                              .withPOSTData (juce::String::fromUTF8 (requestBody (request).c_str()));
    if (! juce::URL::isProbablyAWebsiteURL (request.endpoint))
        return failure (ChatError::Rejected, "Bad endpoint URL in Settings");

    juce::WebInputStream stream (url, true);
    stream.withExtraHeaders ("Content-Type: application/json\r\nAccept: application/json\r\nAuthorization: Bearer "
                             + juce::String (apiKey).trim())
          .withConnectionTimeout (kTimeoutMs);

    // The helper thread does the blocking I/O; this thread watches the
    // cancel flag and the deadline and cancels the stream on either.
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool connected = false;
    int status = 0;
    std::string body, retryAfter;
    std::thread io ([&]
    {
        const bool ok = stream.connect (nullptr);
        int st = 0;
        std::string b, ra;
        if (ok)
        {
            st = stream.getStatusCode();
            ra = stream.getResponseHeaders().getValue ("Retry-After", "").toStdString();
            b = stream.readEntireStreamAsString().toStdString();
        }
        std::lock_guard<std::mutex> lock (m);
        connected = ok;
        status = st;
        body = std::move (b);
        retryAfter = std::move (ra);
        done = true;
        cv.notify_all();
    });

    bool cancelled = false, timedOut = false;
    {
        std::unique_lock<std::mutex> lock (m);
        while (! done)
        {
            cv.wait_for (lock, std::chrono::milliseconds (100));
            if (done)
                break;
            const double t = elapsed();
            if (! cancelled && ! timedOut)
            {
                if (cancel != nullptr && cancel->load())
                    cancelled = true;
                else if (t * 1000.0 > kTimeoutMs)
                    timedOut = true;
                if (cancelled || timedOut)
                {
                    lock.unlock();
                    stream.cancel();
                    lock.lock();
                    continue;
                }
            }
            if (onWaiting && ! cancelled && ! timedOut)
            {
                lock.unlock();
                onWaiting (t);
                lock.lock();
            }
        }
    }
    io.join();

    ChatReply r;
    if (cancelled)
        r = failure (ChatError::Cancelled, "Cancelled");
    else if (timedOut)
        r = failure (ChatError::Timeout, "No reply within " + std::to_string (kTimeoutMs / 1000) + " s");
    else if (! connected || status == 0)
        r = failure (ChatError::Network, "Can't reach " + url.getDomain().toStdString()
                                             + " (network down?)");
    else
        r = interpret (status, body, retryAfter, apiKey);
    r.seconds = elapsed();
    return r;
}

} // namespace vibesynth
