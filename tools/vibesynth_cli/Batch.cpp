// M8: vibesynth_cli --batch prompts.txt (SPEC §6.4, §10.3): how prompt and
// recipe changes are evaluated, and how API credits are spent on purpose.
//
// For each prompt: the plugin's own pipeline (vibesynth::Generator: build ->
// request -> parse -> lint -> compile -> test, self-heal <= 2 retries, the
// weight hint). Into build/batch/<timestamp>/ (or --out DIR):
//   NN_<slug>.pulsar      the accepted patch
//   NN_<slug>.wav         --render of it: the riff phrase for bass prompts,
//                         the default phrase otherwise
//   NN_<slug>_aK.txt      attempt K's raw reply, after a short header
//   summary.txt           everything printed below
// Per prompt: result, attempts, final reason, measured voice ns, tokens,
// seconds. Totals: pass rate first try / within 1 retry / within 2, a
// histogram of failure reasons per stage, the weight hints, total tokens.
// "Passing" counts the first attempt whose patch passed the offline test,
// so a passing patch sent back once by the weight hint still counts at
// the attempt where it passed.
//
// Token use comes from the service's usage field (SPEC §6.4). The key is
// never printed or saved; only where it came from. --endpoint and --model
// override the settings for this run (a local test server, a bad model ID).

#include "Batch.h"

#include <juce_core/juce_core.h>

#include <cstdarg>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "Render.h"
#include "ai/AiSettings.h"
#include "ai/Generator.h"
#include "ai/PromptBuilder.h"
#include "patch/Patch.h"

namespace vibesynth_cli {

namespace {

// Prints and keeps a copy for summary.txt.
struct Log
{
    std::string text;

    void line (const char* fmt, ...)
    {
        char buf[2048];
        va_list args;
        va_start (args, fmt);
        std::vsnprintf (buf, sizeof (buf), fmt, args);
        va_end (args);
        std::printf ("%s\n", buf);
        std::fflush (stdout);
        text += buf;
        text += "\n";
    }
};

std::string slugOf (const std::string& prompt)
{
    std::string s;
    for (const char c : prompt)
    {
        const char l = (c >= 'A' && c <= 'Z') ? static_cast<char> (c - 'A' + 'a') : c;
        if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9'))
            s += l;
        else if (! s.empty() && s.back() != '_')
            s += '_';
        if (s.size() >= 32)
            break;
    }
    while (! s.empty() && s.back() == '_')
        s.pop_back();
    return s.empty() ? "prompt" : s;
}

std::string shorten (const std::string& s, size_t n)
{
    std::string one = juce::String (s).replaceCharacters ("\r\n", "  ").toStdString();
    return one.size() <= n ? one : one.substr (0, n - 3) + "...";
}

// Index of the first attempt whose patch passed the offline test, or -1.
int firstPassing (const vibesynth::GenerationResult& r)
{
    if (! r.ok)
        return -1;
    for (size_t i = 0; i < r.attempts.size(); ++i)
        if (r.attempts[i].stage == "ok" || r.attempts[i].stage == "weight")
            return static_cast<int> (i);
    return -1;
}

double pct (int n, int total)
{
    return total > 0 ? 100.0 * n / total : 0.0;
}

} // namespace

int runBatch (int argc, char** argv, int oversampling)
{
    using namespace vibesynth;
    if (argc < 1)
        return 2;
    const juce::File promptsFile = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (argv[0]));
    int limit = -1;
    juce::File outDir;
    std::string endpoint, model, effort;
    bool effortSet = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string opt = argv[i];
        if (opt == "--batch-limit" && i + 1 < argc)
            limit = juce::String (argv[++i]).getIntValue();
        else if (opt == "--out" && i + 1 < argc)
            outDir = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (argv[++i]));
        else if (opt == "--endpoint" && i + 1 < argc)
            endpoint = argv[++i];
        else if (opt == "--model" && i + 1 < argc)
            model = argv[++i];
        else if (opt == "--effort" && i + 1 < argc)
        {
            effort = argv[++i];
            effortSet = true;
        }
        else
        {
            std::printf ("unknown batch option %s\n", opt.c_str());
            return 2;
        }
    }
    if (! promptsFile.existsAsFile())
    {
        std::printf ("no prompts file %s\n", promptsFile.getFullPathName().toRawUTF8());
        return 2;
    }
    std::vector<std::string> prompts;
    juce::StringArray lines;
    lines.addLines (juce::String (normaliseLineEndings (promptsFile.loadFileAsString().toStdString())));
    for (const auto& l : lines)
        if (l.trim().isNotEmpty() && ! l.trim().startsWith ("#"))
            prompts.push_back (l.trim().toStdString());
    if (limit > 0 && (int) prompts.size() > limit)
        prompts.resize ((size_t) limit);

    const AiSettings settings;
    const std::string key = settings.apiKey();
    if (key.empty())
    {
        std::printf ("no API key: set VIBESYNTH_API_KEY, or save one in Pulsar's Settings (%s)\n",
                     settings.filePath().c_str());
        return 2;
    }
    if (outDir == juce::File())
        outDir = juce::File::getCurrentWorkingDirectory().getChildFile ("build/batch")
                     .getChildFile (juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S"));
    if (! outDir.createDirectory())
    {
        std::printf ("cannot create %s\n", outDir.getFullPathName().toRawUTF8());
        return 2;
    }

    Log log;
    AiConfig cfg = settings.config();
    if (! endpoint.empty())
        cfg.endpoint = endpoint;
    if (! model.empty())
        cfg.generateModel = cfg.retryModel = model;
    if (effortSet)
        cfg.reasoningEffort = effort == "none" ? std::string() : effort;
    log.line ("batch: %d prompts from %s -> %s", (int) prompts.size(), promptsFile.getFileName().toRawUTF8(),
              outDir.getFullPathName().toRawUTF8());
    log.line ("service: %s, model %s @ %.2f, retries %s @ %.2f, reasoning effort %s; key from %s",
              cfg.endpoint.c_str(), cfg.generateModel.c_str(), cfg.generateTemperature, cfg.retryModel.c_str(),
              cfg.retryTemperature, cfg.reasoningEffort.empty() ? "(not sent)" : cfg.reasoningEffort.c_str(),
              settings.keyFromEnvironment() ? AiSettings::kEnvKey : "the settings file");

    const PromptBuilder builder;
    int passAt[Generator::kMaxRetries + 1] = {};
    int failed = 0, weightHints = 0, kept = 0;
    long promptTokens = 0, completionTokens = 0, reasoningTokens = 0, firstRequestTokens = 0;
    int firstRequests = 0;
    double seconds = 0.0;
    std::map<std::string, std::map<std::string, int>> histogram;  // stage -> status -> count
    std::vector<std::string> reasons;

    for (size_t pi = 0; pi < prompts.size(); ++pi)
    {
        const std::string& prompt = prompts[pi];
        char prefix[16];
        std::snprintf (prefix, sizeof (prefix), "%02d", (int) pi + 1);
        const std::string stem = std::string (prefix) + "_" + slugOf (prompt);

        GenerationRequest req;
        req.prompt = prompt;
        req.config = cfg;
        req.apiKey = key;
        const GenerationResult res = Generator::run (req, nullptr);

        for (size_t k = 0; k < res.attempts.size(); ++k)
        {
            const GenerationAttempt& a = res.attempts[k];
            std::string header = "# prompt: " + prompt + "\n# attempt " + std::to_string (k + 1) + ": " + a.stage
                               + " (" + a.status + "), temperature " + juce::String (a.temperature, 2).toStdString()
                               + ", tokens " + std::to_string (a.promptTokens) + " + "
                               + std::to_string (a.completionTokens) + " (" + std::to_string (a.reasoningTokens)
                               + " reasoning), "
                               + juce::String (a.requestSeconds, 1).toStdString() + " s\n";
            if (! a.reason.empty())
                header += "# reason: " + shorten (a.reason, 1000) + "\n";
            for (const auto& n : a.notes)
                header += "# note: " + n + "\n";
            header += "# recipes: " + juce::StringArray ([&] { juce::StringArray s; for (auto& r : a.recipes) s.add (r); return s; }())
                                          .joinIntoString (", ").toStdString()
                    + "; examples: " + juce::StringArray ([&] { juce::StringArray s; for (auto& e : a.examples) s.add (e); return s; }())
                                           .joinIntoString (", ").toStdString() + "\n\n";
            outDir.getChildFile (stem + "_a" + std::to_string (k + 1) + ".txt")
                .replaceWithText (juce::String::fromUTF8 ((header + a.reply).c_str()));
            promptTokens += a.promptTokens;
            completionTokens += a.completionTokens;
            reasoningTokens += a.reasoningTokens;
            if (k == 0 && a.promptTokens > 0)
            {
                firstRequestTokens += a.promptTokens;
                ++firstRequests;
            }
            if (a.stage != "ok")
            {
                ++histogram[a.stage][a.status];
                reasons.push_back (std::string (prefix) + "." + std::to_string (k + 1) + " " + a.stage + ": "
                                   + shorten (a.reason, 220));
            }
            if (a.stage == "weight")
                ++weightHints;
        }
        seconds += res.seconds;

        const int at = firstPassing (res);
        std::string rendered;
        if (at >= 0)
        {
            ++passAt[at];
            if (res.reason.rfind ("kept the heavier", 0) == 0)
                ++kept;
            const juce::File patchFile = outDir.getChildFile (stem + ".pulsar");
            patchFile.replaceWithText (juce::String::fromUTF8 (serializePatch (res.patch).c_str()));
            const juce::File wav = outDir.getChildFile (stem + ".wav");
            const bool riff = builder.isBassPrompt (prompt);
            std::string a0 = patchFile.getFullPathName().toStdString(), a1 = wav.getFullPathName().toStdString();
            std::string a2 = "--phrase", a3 = riff ? "riff" : "default";
            char* rargs[] = { a0.data(), a1.data(), a2.data(), a3.data() };
            const int rc = runRender (4, rargs, oversampling);
            rendered = rc == 0 ? (riff ? "riff" : "default") : "render failed";
        }
        else
        {
            ++failed;
        }

        const int n = static_cast<int> (res.attempts.size());
        log.line ("[%s] %s  attempts %d  ns %4.0f/%-4.0f  tokens %6d + %5d  %5.1f s  %-22s %s%s", prefix,
                  at >= 0 ? "PASS" : "FAIL", n, res.voiceNs, res.voiceBudgetNs, res.promptTokens(),
                  res.completionTokens(), res.seconds, at >= 0 ? shorten (res.patch.name, 22).c_str() : "-",
                  shorten (prompt, 60).c_str(),
                  at >= 0 ? (" [" + rendered + "]" + (res.reason.empty() ? "" : " (" + shorten (res.reason, 120) + ")")).c_str()
                          : ("  <- " + shorten (res.reason, 160)).c_str());
    }

    const int total = static_cast<int> (prompts.size());
    const int first = passAt[0], one = passAt[0] + passAt[1], two = one + passAt[2];
    log.line ("");
    log.line ("== totals (%d prompts) ==", total);
    log.line ("pass first try    %2d/%d  %5.1f %%", first, total, pct (first, total));
    log.line ("pass <= 1 retry   %2d/%d  %5.1f %%", one, total, pct (one, total));
    log.line ("pass <= 2 retries %2d/%d  %5.1f %%   (exit: >= 70 %%)", two, total, pct (two, total));
    log.line ("weight hints %d (heavy patch kept after a failed lighter retry: %d)", weightHints, kept);
    log.line ("failure reasons by stage (every failed attempt):");
    if (histogram.empty())
        log.line ("  (none)");
    for (const auto& [stage, statuses] : histogram)
    {
        int sum = 0;
        std::string parts;
        for (const auto& [status, count] : statuses)
        {
            sum += count;
            parts += (parts.empty() ? "" : ", ") + status + " " + std::to_string (count);
        }
        log.line ("  %-8s %3d  (%s)", stage.c_str(), sum, parts.c_str());
    }
    log.line ("tokens: %ld prompt + %ld completion (%ld of it reasoning) = %ld; first request %ld tokens on average",
              promptTokens, completionTokens, reasoningTokens, promptTokens + completionTokens,
              firstRequests > 0 ? firstRequestTokens / firstRequests : 0L);
    log.line ("time: %.0f s generating (%.1f s per prompt)", seconds, total > 0 ? seconds / total : 0.0);
    log.line ("");
    log.line ("failure details:");
    for (const auto& r : reasons)
        log.line ("  %s", r.c_str());
    outDir.getChildFile ("summary.txt").replaceWithText (juce::String::fromUTF8 (log.text.c_str()));
    return 0;
}

} // namespace vibesynth_cli
