// M11: vibesynth_cli --change / --mutate / --change-batch (SPEC §9.7).
//
// The plugin's own pipelines (vibesynth::Generator with GenerationKind::
// Change, vibesynth::Mutator) against the real service. Every accepted
// patch is saved and rendered with --phrase riff next to it
// (<stem>.pulsar, <stem>.wav), each attempt's raw reply goes to
// <stem>_aK.txt after a short header, and the printed lines (tokens per
// request, seconds, nodes the edit dropped, macro labels it changed) are
// copied to summary.txt for the batch and mutate runs. The key is never
// printed or saved, only where it came from.

#include "Edit.h"

#include <juce_core/juce_core.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "Render.h"
#include "ai/AiSettings.h"
#include "ai/Generator.h"
#include "ai/Mutator.h"
#include "ai/NodeId.h"
#include "patch/Patch.h"
#include "patch/PatchValidator.h"

namespace vibesynth_cli {

namespace {

using namespace vibesynth;

struct Log
{
    std::string text;
    std::mutex lock;

    void line (const char* fmt, ...)
    {
        char buf[2048];
        va_list args;
        va_start (args, fmt);
        std::vsnprintf (buf, sizeof (buf), fmt, args);
        va_end (args);
        std::lock_guard<std::mutex> g (lock);
        std::printf ("%s\n", buf);
        std::fflush (stdout);
        text += buf;
        text += "\n";
    }
};

juce::File cwdFile (const char* path)
{
    return juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (path));
}

std::string shorten (const std::string& s, size_t n)
{
    std::string one = juce::String (s).replaceCharacters ("\r\n", "  ").toStdString();
    return one.size() <= n ? one : one.substr (0, n - 3) + "...";
}

std::string slugOf (const std::string& text)
{
    std::string s;
    for (const char c : text)
    {
        const char l = (c >= 'A' && c <= 'Z') ? static_cast<char> (c - 'A' + 'a') : c;
        if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9'))
            s += l;
        else if (! s.empty() && s.back() != '_')
            s += '_';
        if (s.size() >= 28)
            break;
    }
    while (! s.empty() && s.back() == '_')
        s.pop_back();
    return s.empty() ? "edit" : s;
}

// A .pulsar file, or a built-in by file stem or name ("reese", "Tape Stop
// Bass"). A built-in keeps its stored trim; a file is tested here for its
// trim (as Load does). Its node list comes from that test or a probe.
bool loadBase (const juce::String& arg, Patch& patch, TouchedNodes& nodes, std::string& error)
{
    const juce::String want = arg.endsWithIgnoreCase (".pulsar") ? arg.dropLastCharacters (7) : arg;
    for (int i = 0; i < numBuiltinPresets(); ++i)
    {
        const juce::String stem = juce::String (builtinPresetFile (i)).upToLastOccurrenceOf (".", false, false);
        if (want.equalsIgnoreCase (stem) || want.equalsIgnoreCase (juce::String (builtinPresetName (i))))
        {
            const auto pr = parsePatch (builtinPresetText (i));
            if (! pr.ok())
            {
                error = pr.error;
                return false;
            }
            patch = pr.patch;
            nodes = probeTouchedNodes (patch, 48000.0);
            return true;
        }
    }
    const juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile (arg);
    if (! f.existsAsFile())
    {
        error = "no such patch file or built-in: " + arg.toStdString();
        return false;
    }
    const auto pr = parsePatch (f.loadFileAsString().toStdString());
    if (! pr.ok())
    {
        error = pr.error;
        return false;
    }
    const auto vr = validatePatch (pr.patch, 48000.0);
    if (! vr.ok())
    {
        error = "the base patch fails the offline test: " + vr.error;
        return false;
    }
    patch = pr.patch;
    patch.trim = vr.trim;
    nodes = vr.touched;
    return true;
}

// Common service options; false (printed) on an unknown one. `extra`
// takes the mode's own options and returns how many args it used.
struct ServiceOptions
{
    std::string endpoint, model, effort;
    bool effortSet = false;
};

bool keyAndConfig (const ServiceOptions& o, AiConfig& cfg, std::string& key, std::string& keySource)
{
    const AiSettings settings;
    key = settings.apiKey();
    if (key.empty())
    {
        std::printf ("no API key: set VIBESYNTH_API_KEY, or save one in Pulsar's Settings (%s)\n",
                     settings.filePath().c_str());
        return false;
    }
    keySource = settings.keyFromEnvironment() ? AiSettings::kEnvKey : "the settings file";
    cfg = settings.config();
    if (! o.endpoint.empty())
        cfg.endpoint = o.endpoint;
    if (! o.model.empty())
        cfg.generateModel = cfg.mutateModel = cfg.retryModel = o.model;
    if (o.effortSet)
        cfg.reasoningEffort = o.effort == "none" ? std::string() : o.effort;
    return true;
}

bool serviceOption (int argc, char** argv, int& i, ServiceOptions& o)
{
    const std::string opt = argv[i];
    if (i + 1 >= argc)
        return false;
    if (opt == "--endpoint")
        o.endpoint = argv[++i];
    else if (opt == "--model")
        o.model = argv[++i];
    else if (opt == "--effort")
    {
        o.effort = argv[++i];
        o.effortSet = true;
    }
    else
        return false;
    return true;
}

// Each attempt's raw reply next to the result.
void saveAttempts (const juce::File& dir, const std::string& stem, const std::string& what,
                   const GenerationResult& res)
{
    for (size_t k = 0; k < res.attempts.size(); ++k)
    {
        const GenerationAttempt& a = res.attempts[k];
        std::string header = "# " + what + "\n# attempt " + std::to_string (k + 1) + ": " + a.stage + " ("
                           + a.status + "), temperature " + juce::String (a.temperature, 2).toStdString()
                           + ", tokens " + std::to_string (a.promptTokens) + " + "
                           + std::to_string (a.completionTokens) + " (" + std::to_string (a.reasoningTokens)
                           + " reasoning), " + juce::String (a.requestSeconds, 1).toStdString() + " s\n";
        if (! a.reason.empty())
            header += "# reason: " + shorten (a.reason, 1000) + "\n";
        for (const auto& n : a.notes)
            header += "# note: " + n + "\n";
        std::string recipes;
        for (const auto& r : a.recipes)
            recipes += (recipes.empty() ? "" : ", ") + r;
        header += "# recipes: " + recipes + "\n\n";
        dir.getChildFile (stem + "_a" + std::to_string (k + 1) + ".txt")
            .replaceWithText (juce::String::fromUTF8 ((header + a.reply).c_str()));
    }
}

// Saves an accepted patch and renders the riff next to it.
std::string saveAndRender (const juce::File& patchFile, const Patch& patch, int oversampling)
{
    patchFile.replaceWithText (juce::String::fromUTF8 (serializePatch (patch).c_str()));
    const juce::File wav = patchFile.withFileExtension ("wav");
    std::string a0 = patchFile.getFullPathName().toStdString(), a1 = wav.getFullPathName().toStdString();
    std::string a2 = "--phrase", a3 = "riff";
    char* rargs[] = { a0.data(), a1.data(), a2.data(), a3.data() };
    return runRender (4, rargs, oversampling) == 0 ? "riff" : "render failed";
}

int firstPassing (const GenerationResult& r)
{
    if (! r.ok)
        return -1;
    for (size_t i = 0; i < r.attempts.size(); ++i)
        if (r.attempts[i].stage == "ok" || r.attempts[i].stage == "weight")
            return static_cast<int> (i);
    return -1;
}

std::string joined (const std::vector<std::string>& v)
{
    std::string s;
    for (const auto& x : v)
        s += (s.empty() ? "" : "; ") + x;
    return s;
}

// One "Change this node" through the real service; prints its lines.
GenerationResult changeOnce (const Patch& base, const TouchedNodes& nodes, const std::string& node,
                             const std::string& instruction, const AiConfig& cfg, const std::string& key)
{
    GenerationRequest req;
    req.kind = GenerationKind::Change;
    req.base = base;
    req.baseNodes = nodes;
    req.baseNodesKnown = true;
    req.node = node;
    req.instruction = instruction;
    req.config = cfg;
    req.apiKey = key;
    return Generator::run (req, nullptr);
}

} // namespace

int runChange (int argc, char** argv, int oversampling)
{
    if (argc < 1)
        return 2;
    const juce::String in (argv[0]);
    std::string node, say;
    juce::File out;
    ServiceOptions so;
    for (int i = 1; i < argc; ++i)
    {
        const std::string opt = argv[i];
        if (opt == "--node" && i + 1 < argc)
            node = argv[++i];
        else if (opt == "--say" && i + 1 < argc)
            say = argv[++i];
        else if (opt == "--out" && i + 1 < argc)
            out = cwdFile (argv[++i]);
        else if (! serviceOption (argc, argv, i, so))
        {
            std::printf ("unknown --change option %s\n", opt.c_str());
            return 2;
        }
    }
    if (node.empty() || say.empty() || out == juce::File())
    {
        std::printf ("--change needs --node <id>, --say \"<instruction>\" and --out out.pulsar\n");
        return 2;
    }
    Patch base;
    TouchedNodes nodes;
    std::string error;
    if (! loadBase (in, base, nodes, error))
    {
        std::printf ("%s\n", error.c_str());
        return 2;
    }
    AiConfig cfg;
    std::string key, keySource;
    if (! keyAndConfig (so, cfg, key, keySource))
        return 2;
    out.getParentDirectory().createDirectory();
    std::printf ("change: %s, node %s: \"%s\" (model %s @ %.2f; key from %s)\n", base.name.c_str(), node.c_str(),
                 say.c_str(), cfg.generateModel.c_str(), cfg.generateTemperature, keySource.c_str());

    const GenerationResult res = changeOnce (base, nodes, node, say, cfg, key);
    const std::string stem = out.getFileNameWithoutExtension().toStdString();
    saveAttempts (out.getParentDirectory(), stem, "change " + base.name + " " + node + ": " + say, res);
    for (size_t k = 0; k < res.attempts.size(); ++k)
    {
        const auto& a = res.attempts[k];
        std::printf ("  attempt %d: %-7s tokens %5d + %5d (%d reasoning)  %5.1f s%s%s\n", (int) k + 1, a.stage.c_str(),
                     a.promptTokens, a.completionTokens, a.reasoningTokens, a.requestSeconds,
                     a.reason.empty() ? "" : "  ", shorten (a.reason, 160).c_str());
    }
    if (! res.ok)
    {
        std::printf ("FAIL %s (%.1f s)\n", res.reason.c_str(), res.seconds);
        return 1;
    }
    const std::string rendered = saveAndRender (out, res.patch, oversampling);
    std::printf ("PASS '%s' -> %s [%s], %.1f s, voice %.0f/%.0f ns, tokens %d + %d\n", res.patch.name.c_str(),
                 out.getFullPathName().toRawUTF8(), rendered.c_str(), res.seconds, res.voiceNs, res.voiceBudgetNs,
                 res.promptTokens(), res.completionTokens());
    std::printf ("  nodes dropped: %s\n  labels changed: %s\n", res.lostNodes.empty() ? "none" : res.lostNodes.c_str(),
                 res.labelChanges.empty() ? "none" : joined (res.labelChanges).c_str());
    return 0;
}

int runMutate (int argc, char** argv, int oversampling)
{
    if (argc < 1)
        return 2;
    const juce::String in (argv[0]);
    juce::File outDir;
    ServiceOptions so;
    for (int i = 1; i < argc; ++i)
    {
        const std::string opt = argv[i];
        if (opt == "--outdir" && i + 1 < argc)
            outDir = cwdFile (argv[++i]);
        else if (! serviceOption (argc, argv, i, so))
        {
            std::printf ("unknown --mutate option %s\n", opt.c_str());
            return 2;
        }
    }
    if (outDir == juce::File())
    {
        std::printf ("--mutate needs --outdir DIR\n");
        return 2;
    }
    MutateRequest req;
    std::string error;
    if (! loadBase (in, req.base, req.baseNodes, error))
    {
        std::printf ("%s\n", error.c_str());
        return 2;
    }
    req.baseNodesKnown = true;
    std::string keySource;
    if (! keyAndConfig (so, req.config, req.apiKey, keySource))
        return 2;
    if (! outDir.createDirectory())
    {
        std::printf ("cannot create %s\n", outDir.getFullPathName().toRawUTF8());
        return 2;
    }

    Log log;
    log.line ("mutate: %s -> %s (model %s @ %.2f, retries @ %.2f; key from %s)", req.base.name.c_str(),
              outDir.getFullPathName().toRawUTF8(), req.config.mutateModel.c_str(), req.config.mutateTemperature,
              req.config.retryTemperature, keySource.c_str());
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    const auto& dirs = PromptResources::embedded().mutateDirections;
    const MutateResult mr = Mutator::run (req, nullptr, {}, [&] (int slot, const GenerationResult& r)
    {
        log.line ("  slot %d %-9s finished at %5.1f s: %s", slot + 1, dirs[(size_t) slot].label.c_str(),
                  (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0,
                  r.ok ? ("'" + r.patch.name + "'").c_str() : shorten (r.reason, 100).c_str());
    });

    for (size_t s = 0; s < mr.slots.size(); ++s)
    {
        const GenerationResult& r = mr.slots[s];
        const std::string stem = std::to_string (s + 1) + "_" + mr.directions[s].key;
        saveAttempts (outDir, stem, "mutate " + req.base.name + ": " + mr.directions[s].label, r);
        std::string rendered = "-";
        if (r.ok)
            rendered = saveAndRender (outDir.getChildFile (stem + ".pulsar"), r.patch, oversampling);
        std::string tokens;
        for (const auto& a : r.attempts)
            tokens += (tokens.empty() ? "" : ", ") + std::to_string (a.promptTokens) + "+"
                    + std::to_string (a.completionTokens);
        log.line ("[%d] %-9s %s  attempts %d  %5.1f s  ns %4.0f/%-4.0f  tokens %s  429s %d  %s", (int) s + 1,
                  mr.directions[s].label.c_str(), r.ok ? "PASS" : "FAIL", (int) r.attempts.size(), r.seconds,
                  r.voiceNs, r.voiceBudgetNs, tokens.c_str(), mr.rateLimited[s],
                  r.ok ? ("'" + r.patch.name + "' [" + rendered + "]").c_str()
                       : ("<- " + shorten (r.reason, 160)).c_str());
        if (r.ok)
            log.line ("      dropped: %s; labels: %s", r.lostNodes.empty() ? "none" : r.lostNodes.c_str(),
                      r.labelChanges.empty() ? "unchanged" : joined (r.labelChanges).c_str());
    }
    int requests = 0;
    for (const auto& r : mr.slots)
        requests += (int) r.attempts.size();
    log.line ("== mutate: %d/%d passed, wall time %.1f s, %d requests, tokens %d + %d (%.0f per request)%s",
              mr.passed(), (int) mr.slots.size(), mr.seconds, requests, mr.promptTokens(), mr.completionTokens(),
              requests > 0 ? (double) (mr.promptTokens() + mr.completionTokens()) / requests : 0.0,
              mr.wentSequential ? ", fell back to sequential requests (429)" : "");
    outDir.getChildFile ("summary.txt").replaceWithText (juce::String::fromUTF8 (log.text.c_str()));
    return 0;
}

int runChangeBatch (int argc, char** argv, int oversampling)
{
    if (argc < 1)
        return 2;
    const juce::File casesFile = cwdFile (argv[0]);
    juce::File outDir;
    ServiceOptions so;
    for (int i = 1; i < argc; ++i)
    {
        const std::string opt = argv[i];
        if (opt == "--out" && i + 1 < argc)
            outDir = cwdFile (argv[++i]);
        else if (! serviceOption (argc, argv, i, so))
        {
            std::printf ("unknown --change-batch option %s\n", opt.c_str());
            return 2;
        }
    }
    if (! casesFile.existsAsFile())
    {
        std::printf ("no cases file %s\n", casesFile.getFullPathName().toRawUTF8());
        return 2;
    }
    struct Case { std::string preset, node, instruction; };
    std::vector<Case> cases;
    juce::StringArray lines;
    lines.addLines (juce::String (normaliseLineEndings (casesFile.loadFileAsString().toStdString())));
    for (const auto& l : lines)
    {
        if (l.trim().isEmpty() || l.trim().startsWith ("#"))
            continue;
        juce::StringArray parts;
        parts.addTokens (l, "|", "\"");
        parts.trim();
        if (parts.size() != 3)
        {
            std::printf ("bad line (want preset | node | instruction): %s\n", l.toRawUTF8());
            return 2;
        }
        cases.push_back ({ parts[0].toStdString(), parts[1].toStdString(), parts[2].toStdString() });
    }
    AiConfig cfg;
    std::string key, keySource;
    if (! keyAndConfig (so, cfg, key, keySource))
        return 2;
    if (outDir == juce::File())
        outDir = juce::File::getCurrentWorkingDirectory().getChildFile ("build/change")
                     .getChildFile (juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S"));
    if (! outDir.createDirectory())
    {
        std::printf ("cannot create %s\n", outDir.getFullPathName().toRawUTF8());
        return 2;
    }

    Log log;
    log.line ("change batch: %d cases from %s -> %s", (int) cases.size(), casesFile.getFileName().toRawUTF8(),
              outDir.getFullPathName().toRawUTF8());
    log.line ("service: %s, model %s @ %.2f, retries @ %.2f, reasoning effort %s; key from %s", cfg.endpoint.c_str(),
              cfg.generateModel.c_str(), cfg.generateTemperature, cfg.retryTemperature,
              cfg.reasoningEffort.empty() ? "(not sent)" : cfg.reasoningEffort.c_str(), keySource.c_str());
    int passAt[Generator::kMaxRetries + 1] = {};
    int requests = 0, keptAll = 0;
    long promptTokens = 0, completionTokens = 0;
    double seconds = 0.0;
    std::vector<std::string> notes;
    for (size_t ci = 0; ci < cases.size(); ++ci)
    {
        const Case& c = cases[ci];
        char prefix[16];
        std::snprintf (prefix, sizeof (prefix), "%02d", (int) ci + 1);
        const std::string stem = std::string (prefix) + "_" + slugOf (c.preset + " " + c.instruction);
        Patch base;
        TouchedNodes nodes;
        std::string error;
        GenerationResult res;
        if (! loadBase (juce::String (c.preset), base, nodes, error))
            res.reason = error;
        else
            res = changeOnce (base, nodes, c.node, c.instruction, cfg, key);
        saveAttempts (outDir, stem, "change " + c.preset + " " + c.node + ": " + c.instruction, res);
        for (const auto& a : res.attempts)
        {
            promptTokens += a.promptTokens;
            completionTokens += a.completionTokens;
            ++requests;
            if (a.stage != "ok")
                notes.push_back (std::string (prefix) + " " + a.stage + ": " + shorten (a.reason, 200));
        }
        seconds += res.seconds;
        const int at = firstPassing (res);
        std::string rendered;
        if (at >= 0)
        {
            ++passAt[at];
            keptAll += res.lostNodes.empty() ? 1 : 0;
            rendered = saveAndRender (outDir.getChildFile (stem + ".pulsar"), res.patch, oversampling);
        }
        log.line ("[%s] %s  attempts %d  tokens %6d + %5d  %5.1f s  %-14s %-18s %s%s", prefix, at >= 0 ? "PASS" : "FAIL",
                  (int) res.attempts.size(), res.promptTokens(), res.completionTokens(), res.seconds,
                  shorten (c.preset, 14).c_str(), shorten (c.node, 18).c_str(), shorten (c.instruction, 44).c_str(),
                  at >= 0 ? (" -> '" + res.patch.name + "' [" + rendered + "]").c_str()
                          : ("  <- " + shorten (res.reason, 140)).c_str());
        if (at >= 0)
            log.line ("      dropped: %s; labels: %s", res.lostNodes.empty() ? "none" : res.lostNodes.c_str(),
                      res.labelChanges.empty() ? "unchanged" : joined (res.labelChanges).c_str());
    }
    const int total = (int) cases.size();
    const int first = passAt[0], within = passAt[0] + passAt[1] + passAt[2];
    log.line ("");
    log.line ("== totals (%d cases) ==", total);
    log.line ("pass first try    %d/%d", first, total);
    log.line ("pass <= 2 retries %d/%d", within, total);
    log.line ("kept every original node: %d of %d passing", keptAll, within);
    log.line ("tokens: %ld prompt + %ld completion over %d requests (%.0f + %.0f per request)", promptTokens,
              completionTokens, requests, requests > 0 ? (double) promptTokens / requests : 0.0,
              requests > 0 ? (double) completionTokens / requests : 0.0);
    log.line ("time: %.0f s (%.1f s per case)", seconds, total > 0 ? seconds / total : 0.0);
    if (! notes.empty())
    {
        log.line ("failed attempts:");
        for (const auto& n : notes)
            log.line ("  %s", n.c_str());
    }
    outDir.getChildFile ("summary.txt").replaceWithText (juce::String::fromUTF8 (log.text.c_str()));
    return 0;
}

} // namespace vibesynth_cli
