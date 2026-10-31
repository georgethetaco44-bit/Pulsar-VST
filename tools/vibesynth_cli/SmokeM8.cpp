// M8 smoketests (SPEC §6). See SmokeM8.h. Offline: the client cases talk
// only to sockets on 127.0.0.1 that this file opens and closes.

#include "SmokeM8.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "Bench.h"
#include "CliCheck.h"
#include "ai/Generator.h"
#include "ai/MetaClient.h"
#include "ai/PromptBuilder.h"
#include "ai/ResponseParser.h"
#include "engine/LexiconTable.h"
#include "patch/LexiconLint.h"
#include "patch/LoopNesting.h"
#include "patch/PatchValidator.h"

namespace vibesynth_cli {

namespace {

using namespace vibesynth;

constexpr double kRate = 48000.0;

int countParams (const std::string& params)
{
    if (juce::String (params).trim().isEmpty())
        return 0;
    return 1 + static_cast<int> (std::count (params.begin(), params.end(), ','));
}

// The text between "## <heading>" and the next "## " heading.
juce::String section (const juce::String& doc, const juce::String& heading)
{
    const auto from = doc.fromFirstOccurrenceOf ("\n## " + heading + "\n", false, false);
    const int end = from.indexOf ("\n## ");
    return end >= 0 ? from.substring (0, end) : from;
}

// Bit i set when the code reads m<i+1> (comments and strings skipped).
unsigned macrosRead (const std::string& codeIn)
{
    const std::string code = blankCommentsAndStrings (codeIn);
    auto ident = [] (char c) { return std::isalnum ((unsigned char) c) || c == '_' || c == '.'; };
    unsigned used = 0;
    for (size_t i = 0; i + 1 < code.size(); ++i)
        if ((code[i] == 'm' || code[i] == 'M') && (i == 0 || ! ident (code[i - 1]))
            && code[i + 1] >= '1' && code[i + 1] <= '8' && (i + 2 >= code.size() || ! ident (code[i + 2])))
            used |= 1u << (code[i + 1] - '1');
    return used;
}

bool lintHas (const LintResult& r, const char* needle)
{
    return r.message().find (needle) != std::string::npos;
}

std::string firstEelBlock (const std::string& text)
{
    const auto s = juce::String (text).fromFirstOccurrenceOf ("```eel", false, false)
                       .fromFirstOccurrenceOf ("\n", false, false);
    return s.upToFirstOccurrenceOf ("```", false, false).toStdString();
}

// "lint: <reason> | ok" for a [FAIL] line: which stage each attempt ended
// at and why.
std::string attemptSummary (const GenerationResult& res)
{
    std::string s;
    for (const auto& a : res.attempts)
        s += (s.empty() ? "" : " | ") + a.stage + (a.reason.empty() ? "" : ": " + a.reason);
    return s.empty() ? "no attempts" : s;
}

const char* kReplyOk =
    "Here you go.\n"
    "```json\n{\"name\": \"T\", \"voiceMode\": \"mono\", \"macros\": ["
    "{\"label\": \"A\", \"default\": 0.2}, {\"label\": \"B\", \"default\": 0.3}, {\"label\": \"C\", \"default\": 0.4},"
    "{\"label\": \"D\", \"default\": 0.5}, {\"label\": \"E\", \"default\": 0.6}, {\"label\": \"F\", \"default\": 0.7},"
    "{\"label\": \"G\", \"default\": 0.8}, {\"label\": \"H\", \"default\": 0.9}],"
    " \"graph\": {\"edges\": [[\"osc_saw:0\", \"out\"]]}}\n```\n"
    "```eel init\nph = 0;\n```\n"
    "```eel voice\nout = osc_saw(0, freq) * env_adsr(0, gate, 0.01, 0.3, 0.8, 0.3) * (m1 + m2 + m3 + m4 + m5 + m6 + m7 + m8) / 8;\n```\n"
    "```eel master\noutl = inl; outr = inr;\n```\nEnjoy!\n";

} // namespace

// ---------------------------------------------------------------------------
// SPEC §6.4: api_reference.md documents exactly the registered lexicon
// (signature = arity, slot pool, voice/master), and exactly the EEL2
// built-ins the lint allows.

namespace {

bool checkApiReference (const PromptResources& res)
{
    const juce::String doc = "\n" + juce::String (res.apiReference) + "\n";
    bool ok = check (doc.length() > 1000, "api_reference.md is embedded (%d chars)", doc.length());

    struct Doc { std::string params; int slots; std::string scope; int count = 0; };
    std::map<std::string, Doc> documented;
    juce::StringArray lines;
    lines.addLines (section (doc, "Functions"));
    for (const auto& l : lines)
    {
        if (! l.startsWith ("- `"))
            continue;
        const auto sig = l.fromFirstOccurrenceOf ("`", false, false).upToFirstOccurrenceOf ("`", false, false);
        const auto name = sig.upToFirstOccurrenceOf ("(", false, false).trim().toStdString();
        const auto params = sig.fromFirstOccurrenceOf ("(", false, false).upToLastOccurrenceOf (")", false, false);
        juce::StringArray parts;
        parts.addTokens (l.fromFirstOccurrenceOf ("`", false, false).fromFirstOccurrenceOf ("`", false, false),
                         juce::String::fromUTF8 ("\xc2\xb7"), "");
        parts.trim();
        Doc& d = documented[name];
        d.params = params.toStdString();
        d.slots = parts.size() > 1 && parts[1].startsWith ("no slot") ? 0 : parts.size() > 1 ? parts[1].getIntValue() : -1;
        d.scope = parts.size() > 2 ? parts[2].upToFirstOccurrenceOf (" ", false, false).toStdString() : "";
        ++d.count;
    }

    int matched = 0;
    for (int i = 0; i < numLexiconEntries(); ++i)
    {
        const LexiconEntry& e = lexiconEntry (i);
        const auto it = documented.find (e.name);
        const std::string scope = e.scope == LexiconScope::Master ? "master" : "both";
        const bool good = it != documented.end() && it->second.count == 1 && it->second.params == e.params
                          && countParams (it->second.params) == e.numArgs && it->second.slots == e.numSlots
                          && it->second.scope == scope;
        if (good)
            ++matched;
        else
            ok = check (false, "api_reference documents %s(%s) with %d slots, %s (found: %s)", e.name, e.params,
                        e.numSlots, scope.c_str(),
                        it == documented.end() ? "missing"
                                               : (it->second.params + ", " + std::to_string (it->second.slots)
                                                  + " slots, " + it->second.scope).c_str()) && ok;
    }
    ok = check (matched == numLexiconEntries(), "api_reference: %d/%d registered functions documented with "
                "the right signature, arity, slots and scope", matched, numLexiconEntries()) && ok;
    int stray = 0;
    for (const auto& [name, d] : documented)
        if (findLexiconEntry (name.c_str()) == nullptr)
        {
            ++stray;
            check (false, "api_reference documents %s, which is not registered", name.c_str());
        }
    ok = check (stray == 0, "api_reference documents nothing that isn't registered (%d stray, %d listed)",
                stray, (int) documented.size()) && ok;

    // Built-ins: every `name(args)` in that section is allowed with that
    // arity, and every allowed one is listed.
    const juce::String builtins = section (doc, "EEL2 built-ins");
    std::map<std::string, int> listed;
    for (int pos = builtins.indexOf ("`"); pos >= 0;)
    {
        const int end = builtins.indexOf (pos + 1, "`");
        if (end < 0)
            break;
        const auto sig = builtins.substring (pos + 1, end);
        if (sig.containsChar ('('))
            listed[sig.upToFirstOccurrenceOf ("(", false, false).toStdString()]
                = countParams (sig.fromFirstOccurrenceOf ("(", false, false)
                                   .upToLastOccurrenceOf (")", false, false).toStdString());
        pos = builtins.indexOf (end + 1, "`");
    }
    int builtinOk = 0;
    for (int i = 0; i < numAllowedBuiltins(); ++i)
    {
        const EelBuiltin& b = allowedBuiltin (i);
        const auto it = listed.find (b.name);
        const bool good = it != listed.end() && it->second >= b.minArgs && (b.maxArgs < 0 || it->second <= b.maxArgs);
        builtinOk += good ? 1 : 0;
        if (! good)
            ok = check (false, "api_reference lists the built-in %s with %d-%d args", b.name, b.minArgs, b.maxArgs) && ok;
    }
    ok = check (builtinOk == numAllowedBuiltins() && (int) listed.size() == numAllowedBuiltins(),
                "api_reference: %d/%d allowed EEL2 built-ins listed with a valid arity, %d listed in all",
                builtinOk, numAllowedBuiltins(), (int) listed.size()) && ok;
    return ok;
}

// Every recipe's first ```eel block lints clean.
bool checkRecipes (const PromptResources& res)
{
    const auto& recipes = res.recipes;
    int recipesClean = 0;
    for (const auto& rec : recipes)
    {
        const std::string code = firstEelBlock (rec.text);
        const bool master = code.find ("// master") == 0;
        const LintResult lr = lintSection (code, master ? "master" : "voice");
        if (lr.ok() && ! code.empty())
            ++recipesClean;
        else
            check (false, "recipe %s example lints clean: %s", rec.file.c_str(),
                   code.empty() ? "no ```eel block" : lr.message().c_str());
    }
    return check (recipesClean == (int) recipes.size() && recipes.size() >= 10,
                  "%d/%d recipe examples lint clean", recipesClean, (int) recipes.size());
}

} // namespace

bool smokeApiReference()
{
    return checkApiReference (PromptResources::embedded());
}

// ---------------------------------------------------------------------------
// SPEC §6.4 lint: one case per rule, then the validator path and the
// shipped presets and recipes.

bool smokeLint()
{
    bool ok = true;
    LintResult r = lintSection ("x = osc_wavetable(0, freq);\nout = x;", "voice");
    ok = check (! r.ok() && lintHas (r, "unknown function osc_wavetable") && lintHas (r, "did you mean")
                    && lintHas (r, "osc_saw") && lintHas (r, "osc_* are:") && lintHas (r, "voice line 1"),
                "lint: unknown function with suggestions: \"%s\"", r.message().c_str()) && ok;
    r = lintSection ("y = tanh(x * 3);", "voice");
    ok = check (! r.ok() && lintHas (r, "no tanh: use fx_sat"), "lint: tanh gets a hint: \"%s\"",
                r.message().c_str()) && ok;
    r = lintSection ("a = osc_saw(0);\nb = flt_lp(0, a, 0.5, 0.2, 9);", "voice");
    ok = check (r.issues.size() == 2 && lintHas (r, "osc_saw takes 2 arguments (slot, hz), got 1")
                    && lintHas (r, "line 2: flt_lp takes 4 arguments (slot, in, cutoff, res), got 5"),
                "lint: lexicon arity, too few and too many: \"%s\"", r.message().c_str()) && ok;
    r = lintSection ("y = min(x);\nz = noise(1);", "voice");
    ok = check (r.issues.size() == 2 && lintHas (r, "min takes 2 arguments, got 1") && lintHas (r, "noise takes 0"),
                "lint: built-in and zero-argument arity: \"%s\"", r.message().c_str()) && ok;
    r = lintSection ("a = osc_supersaw(2, freq, 0.5, 0.5);\nb = osc_saw(1.5, freq);\nc = phys_string(1, x, 220, 0.5, 0.5);",
                     "voice");
    ok = check (r.issues.size() == 3 && lintHas (r, "osc_supersaw slot 2 is out of range (it has 2 slots: 0-1)")
                    && lintHas (r, "osc_saw slot 1.5 is out of range")
                    && lintHas (r, "phys_string slot 1 is out of range (it has 1 slot: 0)"),
                "lint: literal slot ranges: \"%s\"", r.message().c_str()) && ok;
    r = lintSection ("k = 1; a = osc_saw(k + 1, freq); b = osc_saw(3, freq);", "voice");
    ok = check (r.ok(), "lint: an expression slot and the last slot pass%s%s", r.ok() ? "" : ": ",
                r.message().c_str()) && ok;
    r = lintSection ("outl = bus_reverb(inl, inr, 0.5, 0.3); outr = bus_reverb_r();", "voice");
    LintResult rm = lintSection ("outl = bus_reverb(inl, inr, 0.5, 0.3); outr = bus_reverb_r(); y = bus_sat(x, 0.2);",
                                 "master");
    LintResult rs = lintSection ("y = bus_sat(x, 0.2);", "voice");
    ok = check (r.issues.size() == 2 && lintHas (r, "bus_reverb works only in master") && rm.ok() && rs.ok(),
                "lint: stateful bus_* only in master, bus_sat anywhere: \"%s\"", r.message().c_str()) && ok;
    r = lintSection ("freembuf(0); stack_push(x);", "voice");
    ok = check (r.issues.size() == 2 && lintHas (r, "freembuf is not allowed"), "lint: refused built-ins: \"%s\"",
                r.message().c_str()) && ok;
    r = lintSection ("function sh(x, k) local(t) ( t = x * k; t / (1 + abs(t)) );\n"
                     "y = sh(osc_saw(0, freq), 3) + this.sh(1, 2);\nz = sh(1);", "voice");
    ok = check (r.issues.size() == 1 && lintHas (r, "line 3: function sh takes 2 arguments, got 1"),
                "lint: user functions (incl. namespaced calls) pass, a wrong user arity fails: \"%s\"",
                r.message().c_str()) && ok;
    r = lintSection ("// osc_bogus(1)\n/* fx_nope(2) */ s = \"x(1, 2)\"; OSC_SAW(0, freq); while (i < 3) (i += 1); "
                     "loop(4, j += 1); y = (a > 0) ? (b) : (c);", "voice");
    ok = check (r.ok(), "lint: comments, strings, case, loop/while and plain parentheses are fine%s%s",
                r.ok() ? "" : ": ", r.message().c_str()) && ok;

    // Functions defined in master are visible in voice (one VM's table).
    Patch p;
    p.voice = "out = shape(osc_saw(0, freq));";
    p.master = "function shape(x) ( fx_sat(x, 0.3) ); outl = inl; outr = inr;";
    ok = check (lintPatch (p).ok(), "lint: a function defined in another section is known") && ok;
    // The validator runs the lint before compiling (generated and loaded patches).
    p.voice = "out = osc_wavetable(0, freq);";
    const ValidationResult vr = validatePatch (p, kRate);
    ok = check (vr.status == PatchStatus::LintError && vr.error.find ("osc_wavetable") != std::string::npos,
                "validator rejects an invented function as lint (%s): \"%s\"", patchStatusName (vr.status),
                vr.error.c_str()) && ok;

    int clean = 0;
    for (int i = 0; i < numBuiltinPresets(); ++i)
    {
        const ParseResult pr = parsePatch (builtinPresetText (i));
        const LintResult lr = pr.ok() ? lintPatch (pr.patch) : LintResult{ { "does not parse" } };
        if (lr.ok())
            ++clean;
        else
            check (false, "%s lints clean: %s", builtinPresetFile (i), lr.message().c_str());
    }
    ok = check (clean == numBuiltinPresets(), "%d/%d built-in presets lint clean", clean, numBuiltinPresets()) && ok;

    ok = checkRecipes (PromptResources::embedded()) && ok;
    return ok;
}

// ---------------------------------------------------------------------------
// SPEC §6.2 parser.

namespace {

std::string withCrlf (const std::string& text)
{
    return juce::String (normaliseLineEndings (text)).replace ("\n", "\r\n").toStdString();
}

// `crlf`: every reply goes in with Windows line endings; the results must
// be the same.
bool checkResponseParser (bool crlf)
{
    auto parseResponse = [crlf] (const std::string& reply)
    {
        return vibesynth::parseResponse (crlf ? withCrlf (reply) : reply);
    };
    bool ok = true;
    ResponseParseResult r = parseResponse (kReplyOk);
    ok = check (r.ok() && r.patch.name == "T" && r.patch.voiceMode == "mono" && r.patch.macros.size() == 8
                    && r.patch.init == "ph = 0;" && r.patch.master == "outl = inl; outr = inr;"
                    && r.patch.voice.find ("out = osc_saw") == 0 && r.patch.graphJson.find ("osc_saw:0") != std::string::npos
                    && r.notes.empty(),
                "parser: the full layout with prose around it (%d notes)", (int) r.notes.size()) && ok;

    r = parseResponse ("```json\n{\"name\": \"N\", \"voiceMode\": \"poly\", \"macros\": []}\n```\n"
                       "```EEL2 Voice\nout = 0;\n```\n");
    ok = check (r.ok() && r.patch.init.empty() && r.patch.master.empty() && r.patch.voice == "out = 0;",
                "parser: ```EEL2 Voice fence, init and master missing") && ok;
    r = parseResponse ("```json\n{\"name\": \"N\"}\n```\n```eel\n// voice\nout = 1;\n```\n");
    ok = check (r.ok() && r.patch.voice.find ("out = 1;") != std::string::npos,
                "parser: an ```eel block named by its first comment") && ok;
    r = parseResponse ("```json\n{\"name\": \"N\"}\n```\n```eel master\nouts = 1;\n```\n");
    ok = check (r.status == PatchStatus::MissingVoice, "parser: no voice block is a parse error: \"%s\"",
                r.error.c_str()) && ok;
    r = parseResponse ("```json\n{\"name\": \"N\", \"macros\": [}\n```\n```eel voice\nout = 0;\n```\n");
    ok = check (r.status == PatchStatus::BadJson && r.error.find ("invalid JSON") == 0,
                "parser: invalid JSON is a parse error: \"%s\"", r.error.c_str()) && ok;
    r = parseResponse ("Sure! out = osc_saw(0, freq);");
    ok = check (r.status == PatchStatus::BadJson && r.error.find ("no ```json") == 0,
                "parser: no JSON block is a parse error: \"%s\"", r.error.c_str()) && ok;
    r = parseResponse ("```json\n{\"name\": \"N\", \"code\": {\"voice\": \"out = osc_saw(0, freq);\"}}\n```\n");
    ok = check (r.status == PatchStatus::BadField && r.error.find ("code inside JSON strings") == 0,
                "parser: code inside JSON strings is refused: \"%s\"", r.error.c_str()) && ok;
    r = parseResponse ("```json\n{\"name\": \"N\", \"voice\": \"x\"}\n```\n```eel voice\nout = 0;\n```\n");
    ok = check (r.ok() && ! r.notes.empty(), "parser: JSON code next to real ```eel blocks is ignored with a note") && ok;
    r = parseResponse ("```json\n{\"name\": \"N\", \"voiceMode\": \"mono\", \"macros\": [{\"label\": \"A\", \"default\": 1.4},"
                       " {\"label\": \"B\", \"default\": -0.2}, \"C\", {\"label\": \"D\", \"default\": 0.5},"
                       " {\"label\": \"E\", \"default\": 0.5},],}\n```\n```eel voice\nout = 0;\n```\n");
    ok = check (r.ok() && r.patch.macros.size() == 8 && r.patch.macros[0].defaultValue == 1.0
                    && r.patch.macros[1].defaultValue == 0.0 && r.patch.macros[2].label == "C"
                    && r.patch.macros[7].label.empty() && r.notes.size() == 3,
                "parser: trailing commas, defaults clamped (1.4 -> 1, -0.2 -> 0), padded 5 -> 8 (%s)",
                juce::StringArray ([&] { juce::StringArray a; for (auto& n : r.notes) a.add (n); return a; }())
                    .joinIntoString ("; ").toRawUTF8()) && ok;
    std::string ten = "```json\n{\"name\": \"N\", \"voiceMode\": \"poly\", \"macros\": [";
    for (int i = 0; i < 10; ++i)
        ten += std::string (i > 0 ? "," : "") + "{\"label\": \"M" + std::to_string (i) + "\", \"default\": 0.5}";
    ten += "]}\n```\n```eel voice\nout = 0;\n```\n";
    r = parseResponse (ten);
    ok = check (r.ok() && r.patch.macros.size() == 8 && r.patch.macros[7].label == "M7" && r.notes.size() == 1
                    && r.notes[0] == "truncated macros 10 -> 8",
                "parser: 10 macros truncated to 8 and reported") && ok;
    std::string big = "```json\n{\"name\": \"N\"}\n```\n```eel voice\n" + std::string (33 * 1024, ' ') + "out = 0;\n```\n";
    r = parseResponse (big);
    ok = check (r.status == PatchStatus::TooLarge, "parser: a 33 KB section is refused: \"%s\"", r.error.c_str()) && ok;
    return ok;
}

} // namespace

bool smokeResponseParser()
{
    return checkResponseParser (false);
}

// ---------------------------------------------------------------------------
// SPEC §6.1: the few-shot examples parse, have 8 labelled macros that the
// code all reads, pass the offline test, and stay lean (cost reported).

namespace {

// `measure`: also report each example's CPU cost.
bool checkPromptExamples (const PromptResources& res, bool measure)
{
    bool ok = true;
    const auto& examples = res.examples;
    std::map<std::string, int> categories;
    for (const auto& e : examples)
    {
        ++categories[e.category];
        const ResponseParseResult pr = parseResponse (e.reply);
        if (! check (pr.ok() && pr.notes.empty(), "example %s parses with no fix-ups%s%s", e.file.c_str(),
                     pr.ok() ? "" : ": ", pr.error.c_str()))
        {
            ok = false;
            continue;
        }
        Patch p = pr.patch;
        p.prompt = e.prompt;
        bool labelled = p.macros.size() == 8;
        for (const auto& m : p.macros)
            labelled = labelled && ! m.label.empty() && m.label.size() <= 12;
        const unsigned read = macrosRead (p.init) | macrosRead (p.voice) | macrosRead (p.master);
        ok = check (labelled && read == 0xFFu && ! e.prompt.empty() && ! e.keywords.empty(),
                    "example %s: prompt, keywords, 8 short labels, reads m1-m8 (mask 0x%02x)", e.file.c_str(), read)
             && ok;
        const ValidationResult vr = validatePatch (p, kRate);
        ok = check (vr.ok(), "example %s passes the offline test%s%s", e.file.c_str(), vr.ok() ? "" : ": ",
                    vr.error.c_str()) && ok;
        if (! measure)
            continue;
        const PresetCost cost = measurePresetCost (p, 3, 24000, PatchProgram::kDefaultOversampling);
        std::printf ("[INFO] example %s (%s): voice %.0f ns (poly budget %.0f), master %.0f ns (budget %.0f)\n",
                     e.file.c_str(), p.voiceMode.c_str(), cost.voiceNs, Patch::kPolyVoiceBudgetNs, cost.masterNs,
                     Patch::kMasterBudgetNs);
    }
    ok = check (examples.size() == 3 && categories["bass"] == 1 && categories["pluck"] == 1
                    && categories["texture"] == 1,
                "3 examples: a bass, a pluck and a texture (%d found)", (int) examples.size()) && ok;
    return ok;
}

} // namespace

bool smokePromptExamples()
{
    return checkPromptExamples (PromptResources::embedded(), true);
}

// ---------------------------------------------------------------------------
// SPEC §6.1 builder: system = prompt + whole API, 2 examples by keyword,
// 2-4 recipes by keyword, deterministic; a retry adds reply + reason.

bool smokePromptBuilder()
{
    const PromptResources& res = PromptResources::embedded();
    const PromptBuilder b;
    bool ok = check (! res.systemPrompt.empty() && res.recipes.size() >= 10 && res.examples.size() == 3,
                     "prompt resources: system prompt, %d recipes, %d examples", (int) res.recipes.size(),
                     (int) res.examples.size());

    const BuiltPrompt p = b.build ("wobbly neuro reese bass with a stutter");
    ok = check (p.messages.size() == 6 && p.messages[0].role == "system"
                    && p.messages[0].content == res.systemPrompt + "\n\n" + res.apiReference
                    && p.messages[1].role == "user" && p.messages[2].role == "assistant"
                    && p.messages[5].role == "user"
                    && p.messages[5].content.find ("Write a Pulsar patch for: wobbly neuro reese bass with a stutter")
                           != std::string::npos,
                "builder: system (prompt + API), 2 example pairs, then recipes + the prompt") && ok;
    const BuiltPrompt again = b.build ("wobbly neuro reese bass with a stutter");
    bool same = again.messages.size() == p.messages.size() && again.recipeFiles == p.recipeFiles;
    for (size_t i = 0; same && i < p.messages.size(); ++i)
        same = again.messages[i].content == p.messages[i].content;
    ok = check (same, "builder: deterministic for a given prompt") && ok;
    ok = check (p.exampleFiles.size() == 2 && p.exampleFiles[0] == "example_bass.md" && p.recipeFiles.size() == 4
                    && b.isBassPrompt ("wobbly neuro reese bass"),
                "builder: a bass prompt gets the bass example first and 4 recipes (%s, %s / %s %s %s %s)",
                p.exampleFiles[0].c_str(), p.exampleFiles.size() > 1 ? p.exampleFiles[1].c_str() : "-",
                p.recipeFiles[0].c_str(), p.recipeFiles.size() > 1 ? p.recipeFiles[1].c_str() : "-",
                p.recipeFiles.size() > 2 ? p.recipeFiles[2].c_str() : "-",
                p.recipeFiles.size() > 3 ? p.recipeFiles[3].c_str() : "-") && ok;
    const BuiltPrompt pad = b.build ("lush evolving pad");
    const BuiltPrompt keys = b.build ("bright piano chords");
    const BuiltPrompt none = b.build ("something nice");
    ok = check (pad.exampleFiles[0] == "example_texture.md" && keys.exampleFiles[0] == "example_pluck.md"
                    && ! b.isBassPrompt ("lush evolving pad"),
                "builder: a pad prompt gets the texture example, a keys prompt the pluck") && ok;
    ok = check (none.recipeFiles.size() == 2 && none.recipeFiles[0] == "ott_glue.md" && pad.recipeFiles.size() >= 2,
                "builder: a prompt with no keywords still gets 2 recipes (%s, %s)", none.recipeFiles[0].c_str(),
                none.recipeFiles.size() > 1 ? none.recipeFiles[1].c_str() : "-") && ok;

    const RetryContext rc { "previous reply", "compile error in voice: syntax error line 3" };
    const BuiltPrompt r = b.build ("wobbly neuro reese bass with a stutter", &rc);
    ok = check (r.messages.size() == 8 && r.messages[6].role == "assistant" && r.messages[6].content == "previous reply"
                    && r.messages[7].role == "user"
                    && r.messages[7].content.find ("compile error in voice: syntax error line 3") != std::string::npos,
                "builder: a retry adds the previous reply and the exact reason") && ok;

    int total = 0;
    for (const auto& m : p.messages)
        total += estimateTokens (m.content);
    std::printf ("[INFO] request size estimate (~4 chars/token): system %d + API %d, total for this prompt %d tokens\n",
                 estimateTokens (res.systemPrompt), estimateTokens (res.apiReference), total);
    return ok;
}

// ---------------------------------------------------------------------------
// SPEC §6.3/§6.6 client: body, error mapping, no key, network down, cancel.

bool smokeClient()
{
    bool ok = true;
    ChatRequest req;
    req.endpoint = "http://127.0.0.1:9/v1/chat/completions";
    req.model = "m";
    req.temperature = 0.15;
    req.reasoningEffort = "low";
    req.messages = { { "user", "hi \"there\"\n" } };
    const std::string body = MetaClient::requestBody (req);
    ok = check (body.find ("\"model\":\"m\"") != std::string::npos && body.find ("\"temperature\":0.15") != std::string::npos
                    && body.find ("\"reasoning_effort\":\"low\"") != std::string::npos
                    && body.find ("Bearer") == std::string::npos && body.find ("\\\"there\\\"") != std::string::npos,
                "client: request body (model, temperature, escaped messages, no key): %s", body.c_str()) && ok;

    const std::string key = "sk-test-SECRET-123456";
    ChatReply r = MetaClient::interpret (401, "{\"error\": {\"message\": \"bad key " + key + "\"}}", "", key);
    ok = check (r.error == ChatError::Auth && r.message.find ("SECRET") == std::string::npos,
                "client: 401 -> \"%s\"", r.message.c_str()) && ok;
    r = MetaClient::interpret (429, "{}", "7", key);
    ok = check (r.error == ChatError::RateLimited && r.retryAfterSeconds == 7 && r.message.find ("7 s") != std::string::npos,
                "client: 429 + Retry-After 7 -> \"%s\"", r.message.c_str()) && ok;
    r = MetaClient::interpret (503, "<html>", "", key);
    ok = check (r.error == ChatError::Server, "client: 503 -> \"%s\"", r.message.c_str()) && ok;
    r = MetaClient::interpret (404, "{\"error\": {\"message\": \"model 'x' not found for " + key + "\"}}", "", key);
    ok = check (r.error == ChatError::Rejected && r.message.find ("not found") != std::string::npos
                    && r.message.find ("SECRET") == std::string::npos,
                "client: 404 -> \"%s\" (key scrubbed)", r.message.c_str()) && ok;
    r = MetaClient::interpret (200, "", "", key);
    ChatReply g = MetaClient::interpret (200, "not json {", "", key);
    ChatReply e = MetaClient::interpret (200, "{\"choices\": [{\"message\": {\"content\": \"\"}}]}", "", key);
    ok = check (r.error == ChatError::BadReply && g.error == ChatError::BadReply && e.error == ChatError::BadReply,
                "client: empty, garbled and empty-content replies -> \"%s\" / \"%s\" / \"%s\"", r.message.c_str(),
                g.message.c_str(), e.message.c_str()) && ok;
    ChatReply len = MetaClient::interpret (200, "{\"choices\": [{\"finish_reason\": \"length\", \"message\": {\"content\": \"\"}}],"
                                               " \"usage\": {\"prompt_tokens\": 5000, \"completion_tokens\": 4096,"
                                               " \"completion_tokens_details\": {\"reasoning_tokens\": 4096}}}", "", key);
    ok = check (len.error == ChatError::BadReply && len.reasoningTokens == 4096
                    && len.message.find ("token limit while reasoning") != std::string::npos,
                "client: empty reply at the length limit -> \"%s\"", len.message.c_str()) && ok;
    r = MetaClient::interpret (200, "{\"choices\": [{\"message\": {\"role\": \"assistant\", \"content\": \"hello\"}}],"
                                    " \"usage\": {\"prompt_tokens\": 120, \"completion_tokens\": 34}}", "", key);
    ok = check (r.ok() && r.content == "hello" && r.promptTokens == 120 && r.completionTokens == 34,
                "client: 200 -> content and usage tokens (%d + %d)", r.promptTokens, r.completionTokens) && ok;

    std::atomic<bool> cancel { false };
    r = MetaClient::send (req, "", &cancel);
    ok = check (r.error == ChatError::NoKey, "client: no key -> \"%s\" (no request sent)", r.message.c_str()) && ok;

    // Network down: a port nothing listens on.
    int freePort = 0;
    {
        juce::StreamingSocket probe;
        if (probe.createListener (0, "127.0.0.1"))
            freePort = probe.getBoundPort();
    }
    req.endpoint = "http://127.0.0.1:" + std::to_string (freePort) + "/v1/chat/completions";
    r = MetaClient::send (req, key, &cancel);
    ok = check (r.error == ChatError::Network && r.seconds < 10.0, "client: connection refused -> \"%s\" in %.2f s",
                r.message.c_str(), r.seconds) && ok;

    // Cancel: a server that accepts and never answers.
    juce::StreamingSocket listener;
    if (! check (listener.createListener (0, "127.0.0.1"), "client: local test listener"))
        return false;
    std::thread server ([&listener]
    {
        std::vector<std::unique_ptr<juce::StreamingSocket>> held;
        while (auto* c = listener.waitForNextConnection())
            held.emplace_back (c);
    });
    req.endpoint = "http://127.0.0.1:" + std::to_string (listener.getBoundPort()) + "/v1/chat/completions";
    std::thread canceller ([&cancel]
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (400));
        cancel = true;
    });
    int waits = 0;
    r = MetaClient::send (req, key, &cancel, [&waits] (double) { ++waits; });
    canceller.join();
    listener.close();
    server.join();
    ok = check (r.error == ChatError::Cancelled && r.seconds < 1.5 && waits >= 2,
                "client: cancel during a silent request returns in %.2f s (%d progress calls): \"%s\"",
                r.seconds, waits, r.message.c_str()) && ok;
    return ok;
}

// ---------------------------------------------------------------------------
// SPEC §6.4 self-heal on canned replies: lint failure then a good patch,
// three bad replies, a request error, and the weight hint.

bool smokeSelfHeal()
{
    bool ok = true;
    const std::string good = PromptResources::embedded().examples.at (0).reply;
    const std::string invented = juce::String (good).replace ("osc_supersaw(0,", "osc_wavetable(0,").toStdString();

    // The weight hint reads an injected voice cost, not a timing, so these
    // cases run the same on any machine (Rosetta included): a patch marked
    // "heavy_marker" costs 100x its budget, anything else a tenth of it.
    std::vector<ChatRequest> seen;
    std::vector<std::string> replies;
    GenerationRequest gr;
    gr.prompt = "dark reese";
    gr.measureVoice = [] (const Patch& p)
    {
        const bool heavy = p.voice.find ("heavy_marker") != std::string::npos;
        return p.voiceBudgetNs() * (heavy ? 100.0 : 0.1);
    };
    gr.transport = [&] (const ChatRequest& c)
    {
        seen.push_back (c);
        ChatReply r;
        r.content = replies.at (std::min (seen.size(), replies.size()) - 1);
        r.promptTokens = 1000;
        r.completionTokens = 400;
        return r;
    };

    replies = { invented, good };
    GenerationResult res = Generator::run (gr, nullptr);
    const std::string retryText = seen.size() == 2 ? seen[1].messages.back().content : "";
    ok = check (res.ok && res.attempts.size() == 2 && res.attempts[0].stage == "lint" && res.attempts[1].stage == "ok"
                    && seen.size() == 2 && seen[1].temperature == 0.15
                    && seen[1].messages[seen[1].messages.size() - 2].content == invented
                    && retryText.find ("unknown function osc_wavetable") != std::string::npos
                    && res.patch.prompt == "dark reese" && res.patch.trim > 0.0 && res.patch.trim <= 1.0
                    && res.promptTokens() == 2000 && res.completionTokens() == 800,
                "self-heal: lint failure -> retry at 0.15 with the reply and reason -> passes (%d attempts, trim %.3f: %s)",
                (int) res.attempts.size(), res.patch.trim, attemptSummary (res).c_str()) && ok;

    seen.clear();
    replies = { "no blocks at all" };
    res = Generator::run (gr, nullptr);
    ok = check (! res.ok && res.attempts.size() == 3 && seen.size() == 3 && res.attempts[2].stage == "parse"
                    && res.reason.find ("no ```json") == 0,
                "self-heal: 3 bad replies -> fails after 2 retries: \"%s\" (%s)", res.reason.c_str(),
                attemptSummary (res).c_str()) && ok;

    seen.clear();
    gr.transport = [&] (const ChatRequest& c)
    {
        seen.push_back (c);
        ChatReply r;
        r.error = ChatError::Auth;
        r.message = "API key rejected (HTTP 401): check it in Settings";
        return r;
    };
    res = Generator::run (gr, nullptr);
    ok = check (! res.ok && res.attempts.size() == 1 && res.attempts[0].stage == "request" && seen.size() == 1,
                "self-heal: a request error is not retried: \"%s\" (%s)", res.reason.c_str(),
                attemptSummary (res).c_str()) && ok;

    // Weight hint: the poly pluck, marked heavy (the injected cost above).
    const std::string heavy = juce::String (PromptResources::embedded().examples.at (1).reply)
                                  .replace ("out = fx_sat(x, 0.25) * env * vel;",
                                            "heavy_marker = 1;\nout = fx_sat(x, 0.25) * env * vel * heavy_marker;")
                                  .toStdString();
    const std::string light = PromptResources::embedded().examples.at (1).reply;
    seen.clear();
    gr.transport = [&] (const ChatRequest& c)
    {
        seen.push_back (c);
        ChatReply r;
        r.content = replies.at (std::min (seen.size(), replies.size()) - 1);
        return r;
    };
    replies = { heavy, light };
    res = Generator::run (gr, nullptr);
    ok = check (res.ok && res.attempts.size() == 2 && res.attempts[0].stage == "weight"
                    && res.attempts[0].reason == "too heavy: 30000 ns per voice-sample, budget 300 (poly); simplify"
                    && res.patch.voice.find ("heavy_marker") == std::string::npos,
                "self-heal: weight hint \"%s\" -> the lighter retry is kept (%.0f ns; %d attempts: %s)",
                res.attempts.empty() ? "" : res.attempts[0].reason.c_str(), res.voiceNs, (int) res.attempts.size(),
                attemptSummary (res).c_str()) && ok;
    seen.clear();
    replies = { heavy, "broken", "broken" };
    res = Generator::run (gr, nullptr);
    ok = check (res.ok && res.attempts.size() == 3 && res.patch.voice.find ("heavy_marker") != std::string::npos
                    && res.reason.find ("kept the heavier version") == 0,
                "self-heal: if the lighter retries fail, the heavy patch that passed is accepted: \"%s\" (%s)",
                res.reason.c_str(), attemptSummary (res).c_str()) && ok;

    // Cancel before the first request.
    std::atomic<bool> cancel { true };
    res = Generator::run (gr, &cancel);
    ok = check (res.cancelled && ! res.ok, "self-heal: a cancelled run reports cancelled") && ok;
    return ok;
}

// ---------------------------------------------------------------------------
// Windows checkouts and editors: the resource, reply and .pulsar parsers
// read CRLF text exactly as they read LF.

bool smokeCrlf()
{
    auto files = PromptResources::embeddedFiles();
    int converted = 0;
    for (auto& [name, text] : files)
    {
        text = withCrlf (text);
        converted += text.find ("\r\n") != std::string::npos ? 1 : 0;
    }
    const PromptResources crlf = PromptResources::fromFiles (files);
    const PromptResources& lf = PromptResources::embedded();

    CheckPrefix prefix ("CRLF ");
    bool ok = check (converted == (int) files.size() && converted > 0, "%d/%d prompt resources converted to CRLF",
                     converted, (int) files.size());
    bool same = crlf.systemPrompt == lf.systemPrompt && crlf.apiReference == lf.apiReference
                && crlf.recipes.size() == lf.recipes.size() && crlf.examples.size() == lf.examples.size();
    std::string differ;
    for (size_t i = 0; same && i < lf.recipes.size(); ++i)
        if (crlf.recipes[i].text != lf.recipes[i].text || crlf.recipes[i].keywords != lf.recipes[i].keywords
            || crlf.recipes[i].title != lf.recipes[i].title)
            differ += " " + lf.recipes[i].file;
    for (size_t i = 0; same && i < lf.examples.size(); ++i)
        if (crlf.examples[i].reply != lf.examples[i].reply || crlf.examples[i].prompt != lf.examples[i].prompt
            || crlf.examples[i].keywords != lf.examples[i].keywords
            || crlf.examples[i].category != lf.examples[i].category)
            differ += " " + lf.examples[i].file;
    ok = check (same && differ.empty(), "prompt resources parse the same as LF%s%s",
                differ.empty() ? (same ? "" : " (system prompt, API reference or counts differ)") : ": differ in",
                differ.c_str()) && ok;

    ok = checkApiReference (crlf) && ok;
    ok = checkRecipes (crlf) && ok;
    ok = checkPromptExamples (crlf, false) && ok;
    ok = checkResponseParser (true) && ok;

    // A .pulsar saved with CRLF line endings, its code strings too.
    const std::string text = builtinPresetText (0);
    const std::string crlfText = juce::String (withCrlf (text)).replace ("\\n", "\\r\\n").toStdString();
    const ParseResult a = parsePatch (text);
    const ParseResult b = parsePatch (crlfText);
    ok = check (a.ok() && b.ok() && a.patch == b.patch && crlfText.find ("\\r\\n") != std::string::npos,
                ".pulsar %s with CRLF lines and \\r\\n in its code reads back identically%s%s",
                builtinPresetFile (0), b.ok() ? "" : ": ", b.error.c_str()) && ok;
    return ok;
}

} // namespace vibesynth_cli
