// Lexicon lint (SPEC §6.4 M8). See LexiconLint.h for the rules.

#include "patch/LexiconLint.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>

#include "engine/LexiconTable.h"
#include "patch/LoopNesting.h"

namespace vibesynth {

namespace {

// EEL2 built-ins a patch may call (see nseel-compiler.c's function table).
// Left out on purpose: freembuf (frees mem[] blocks, so the next access
// would allocate on the audio thread), the stack_* functions, __memtop,
// __dbg_getstackptr and mem_insert_shuffle (not taught; nothing needs them).
const EelBuiltin kBuiltins[] = {
    { "sin", 1, 1 }, { "cos", 1, 1 }, { "tan", 1, 1 },
    { "asin", 1, 1 }, { "acos", 1, 1 }, { "atan", 1, 1 }, { "atan2", 2, 2 },
    { "sqrt", 1, 1 }, { "sqr", 1, 1 }, { "invsqrt", 1, 1 }, { "pow", 2, 2 },
    { "exp", 1, 1 }, { "log", 1, 1 }, { "log10", 1, 1 },
    { "abs", 1, 1 }, { "sign", 1, 1 }, { "min", 2, 2 }, { "max", 2, 2 },
    { "floor", 1, 1 }, { "ceil", 1, 1 }, { "rand", 1, 1 }, { "sigmoid", 2, 2 },
    { "band", 2, 2 }, { "bor", 2, 2 },
    { "loop", 2, 2 }, { "while", 1, 1 }, { "exec2", 2, 2 }, { "exec3", 3, 3 },
    { "memset", 3, 3 }, { "memcpy", 3, 3 }, { "mem_set_values", 2, -1 },
    { "mem_get_values", 2, -1 }, { "mem_multiply_sum", 3, 3 },
};
constexpr int kNumBuiltins = static_cast<int> (sizeof (kBuiltins) / sizeof (kBuiltins[0]));

const char* const kRefused[] = { "freembuf", "stack_push", "stack_pop", "stack_peek",
                                 "stack_exch", "__memtop", "__dbg_getstackptr",
                                 "mem_insert_shuffle", "gmem" };

// Words that take parentheses but aren't calls.
const char* const kKeywords[] = { "function", "local", "static", "instance", "globals", "global" };

// What to write instead of common functions EEL2 doesn't have.
struct Hint
{
    const char* name;
    const char* text;
};
const Hint kHints[] = {
    { "tanh", "no tanh: use fx_sat(x, drive) or x / (1 + abs(x))" },
    { "sinh", "no sinh: use (exp(x) - exp(-x)) / 2" },
    { "cosh", "no cosh: use (exp(x) + exp(-x)) / 2" },
    { "clamp", "no clamp: use min(max(x, lo), hi)" },
    { "clip", "no clip: use min(max(x, -1), 1)" },
    { "round", "no round: use floor(x + 0.5)" },
    { "int", "no int: use floor(x)" },
    { "trunc", "no trunc: use floor(x) (x >= 0)" },
    { "fmod", "no fmod: use the % operator (integers) or x - floor(x / y) * y" },
    { "mod", "no mod: use the % operator (integers) or x - floor(x / y) * y" },
    { "fract", "no fract: use x - floor(x)" },
    { "frac", "no frac: use x - floor(x)" },
    { "lerp", "no lerp: use a + (b - a) * t" },
    { "mix", "no mix: use a + (b - a) * t" },
    { "log2", "no log2: use log(x) / log(2)" },
    { "exp2", "no exp2: use pow(2, x)" },
    { "lfo", "no lfo: use osc_sine(slot, hz), or sin(beat * 2 * $pi * n) for tempo sync" },
    { "osc_lfo", "no osc_lfo: use osc_sine(slot, hz), or sin(beat * 2 * $pi * n) for tempo sync" },
    { "osc_noise", "use noise() (no arguments)" },
    { "osc_pulse", "use osc_square(slot, hz, pw)" },
    { "fx_dist", "use fx_sat(in, drive) or fx_fold(in, amount)" },
    { "fx_drive", "use fx_sat(in, drive) or fx_fold(in, amount)" },
    { "fx_distort", "use fx_sat(in, drive) or fx_fold(in, amount)" },
    { "fx_reverb", "reverb is bus_reverb(l, r, size, mix), master only" },
    { "fx_chorus", "chorus is bus_chorus(l, r, depth, mix), master only" },
    { "fx_delay", "no delay function: write a mem[] ring buffer, or use fx_comb" },
    { "delay", "no delay function: write a mem[] ring buffer, or use fx_comb" },
    { "util_dc_block", "no DC blocker function: dc += (y - dc) * 20 / srate; y -= dc;" },
    { "dcblock", "no DC blocker function: dc += (y - dc) * 20 / srate; y -= dc;" },
};

bool isIdentStart (char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c == '#';
}

bool isIdentChar (char c)
{
    return isIdentStart (c) || (c >= '0' && c <= '9') || c == '.';
}

bool isSpace (char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string lower (std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char> (c - 'A' + 'a');
    return s;
}

std::string trim (const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && isSpace (s[a]))
        ++a;
    while (b > a && isSpace (s[b - 1]))
        --b;
    return s.substr (a, b - a);
}

const EelBuiltin* findBuiltin (const std::string& name)
{
    for (const EelBuiltin& b : kBuiltins)
        if (name == b.name)
            return &b;
    return nullptr;
}

template <typename List>
bool contains (const List& list, const std::string& name)
{
    for (const char* s : list)
        if (name == s)
            return true;
    return false;
}

int editDistance (const std::string& a, const std::string& b)
{
    std::vector<int> prev (b.size() + 1), cur (b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j)
        prev[j] = static_cast<int> (j);
    for (size_t i = 1; i <= a.size(); ++i)
    {
        cur[0] = static_cast<int> (i);
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min ({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1) });
        std::swap (prev, cur);
    }
    return prev[b.size()];
}

// "osc_saw(slot, hz)"
std::string signature (const LexiconEntry& e)
{
    return std::string (e.name) + "(" + e.params + ")";
}

// The suggestion part of an unknown-function message.
std::string suggestFor (const std::string& name)
{
    for (const Hint& h : kHints)
        if (name == h.name)
            return h.text;

    // Closest names, lexicon first (that's what the model meant to call).
    std::vector<std::pair<int, std::string>> near;
    const int limit = std::max (2, static_cast<int> (name.size()) / 3);
    for (int i = 0; i < numLexiconEntries(); ++i)
    {
        const std::string candidate = lexiconEntry (i).name;
        const int d = editDistance (name, candidate);
        if (d <= limit)
            near.emplace_back (d, candidate);
    }
    for (const EelBuiltin& b : kBuiltins)
    {
        const int d = editDistance (name, b.name);
        if (d <= std::min (limit, 2))
            near.emplace_back (d + 1, b.name);
    }
    std::sort (near.begin(), near.end());

    // Nothing close but a known family ("osc_wavetable"): its nearest members.
    const size_t us = name.find ('_');
    const std::string prefix = us != std::string::npos && us > 0 ? name.substr (0, us + 1) : std::string();
    if (near.empty() && ! prefix.empty())
    {
        for (int i = 0; i < numLexiconEntries(); ++i)
        {
            const std::string candidate = lexiconEntry (i).name;
            if (candidate.compare (0, prefix.size(), prefix) == 0)
                near.emplace_back (editDistance (name, candidate), candidate);
        }
        std::sort (near.begin(), near.end());
        if (near.size() > 2)
            near.resize (2);
    }

    std::string text;
    if (! near.empty())
    {
        text = "did you mean ";
        for (size_t i = 0; i < near.size() && i < 3; ++i)
            text += (i > 0 ? ", " : "") + near[i].second;
        text += "?";
    }

    // The whole family when the prefix is a lexicon one ("osc_*: ...").
    if (! prefix.empty())
    {
        std::string family;
        for (int i = 0; i < numLexiconEntries(); ++i)
        {
            const LexiconEntry& e = lexiconEntry (i);
            if (std::string (e.name).compare (0, prefix.size(), prefix) == 0)
                family += (family.empty() ? "" : ", ") + signature (e);
        }
        if (! family.empty())
            text += (text.empty() ? "" : " ") + prefix + "* are: " + family;
    }
    if (text.empty())
        text = "only the functions in the API reference exist";
    return text;
}

int lineOf (const std::string& s, size_t pos)
{
    return 1 + static_cast<int> (std::count (s.begin(), s.begin() + static_cast<std::ptrdiff_t> (std::min (pos, s.size())), '\n'));
}

struct Call
{
    std::string name;            // lower case
    size_t pos = 0;              // of the name
    bool closed = false;         // matching ')' found
    std::vector<std::string> args;
};

// The call's arguments: top-level commas between '(' at `open` and its
// matching ')'. Parentheses and brackets nest.
void readArgs (const std::string& s, size_t open, Call& call)
{
    int depth = 0;
    std::string current;
    for (size_t i = open; i < s.size(); ++i)
    {
        const char c = s[i];
        if (c == '(' || c == '[')
        {
            if (depth++ > 0)
                current += c;
            continue;
        }
        if (c == ')' || c == ']')
        {
            if (--depth == 0)
            {
                call.closed = true;
                const std::string last = trim (current);
                if (! last.empty() || ! call.args.empty())
                    call.args.push_back (last);
                return;
            }
            current += c;
            continue;
        }
        if (c == ',' && depth == 1)
        {
            call.args.push_back (trim (current));
            current.clear();
            continue;
        }
        current += c;
    }
}

struct Scan
{
    std::vector<Call> calls;
    std::map<std::string, int> functions;  // user function -> parameter count
};

void scanSection (const std::string& code, Scan& scan)
{
    const std::string s = blankCommentsAndStrings (code);
    size_t i = 0;
    bool afterFunctionKeyword = false;
    while (i < s.size())
    {
        if (! isIdentStart (s[i]) || (i > 0 && isIdentChar (s[i - 1])))
        {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < s.size() && isIdentChar (s[i]))
            ++i;
        const std::string word = lower (s.substr (start, i - start));
        size_t j = i;
        while (j < s.size() && isSpace (s[j]))
            ++j;
        const bool isCall = j < s.size() && s[j] == '(';
        if (word == "function")
        {
            afterFunctionKeyword = true;
            continue;
        }
        if (! isCall)
        {
            afterFunctionKeyword = false;
            continue;
        }
        Call call;
        call.name = word;
        call.pos = start;
        readArgs (s, j, call);
        if (afterFunctionKeyword)
        {
            // A definition: "function name(a, b)". Namespaced definitions
            // are called by their last component.
            const size_t dot = word.rfind ('.');
            scan.functions[dot == std::string::npos ? word : word.substr (dot + 1)]
                = static_cast<int> (call.args.size());
            afterFunctionKeyword = false;
            continue;
        }
        if (! contains (kKeywords, word))
            scan.calls.push_back (std::move (call));
    }
}

// Literal slot argument: a plain number (no expression). Returns false if
// `arg` isn't one.
bool literalNumber (const std::string& arg, double& value)
{
    if (arg.empty())
        return false;
    for (const char c : arg)
        if (! ((c >= '0' && c <= '9') || c == '.'))
            return false;
    char* end = nullptr;
    value = std::strtod (arg.c_str(), &end);
    return end != nullptr && *end == 0;
}

void addIssue (LintResult& r, const std::string& section, const std::string& code,
               size_t pos, const std::string& text)
{
    if (static_cast<int> (r.issues.size()) >= LintResult::kMaxIssues)
        return;
    const std::string issue = section + " line " + std::to_string (lineOf (code, pos)) + ": " + text;
    if (std::find (r.issues.begin(), r.issues.end(), issue) == r.issues.end())
        r.issues.push_back (issue);
}

std::string plural (int n, const char* word)
{
    return std::to_string (n) + " " + word + (n == 1 ? "" : "s");
}

void checkCalls (const std::string& code, const std::string& section, const Scan& own,
                 const std::map<std::string, int>& functions, LintResult& r)
{
    const bool master = section == "master";
    std::set<std::string> unknownSeen;
    for (const Call& call : own.calls)
    {
        const size_t dot = call.name.rfind ('.');
        const std::string last = dot == std::string::npos ? call.name : call.name.substr (dot + 1);

        const auto fn = functions.find (last);
        if (fn != functions.end())
        {
            const int n = static_cast<int> (call.args.size());
            if (call.closed && n != fn->second)
                addIssue (r, section, code, call.pos,
                          "function " + last + " takes " + plural (fn->second, "argument") + ", got "
                              + std::to_string (n));
            continue;
        }

        if (const LexiconEntry* e = findLexiconEntry (call.name.c_str()))
        {
            const int n = static_cast<int> (call.args.size());
            if (call.closed && n != e->numArgs)
                addIssue (r, section, code, call.pos,
                          std::string (e->name) + " takes " + plural (e->numArgs, "argument") + " ("
                              + e->params + "), got " + std::to_string (n));
            if (e->scope == LexiconScope::Master && ! master)
                addIssue (r, section, code, call.pos,
                          std::string (e->name) + " works only in master; in voice use fx_sat, "
                              "util_xover_lo or plain code, and put bus effects in master");
            double slot = 0.0;
            if (e->numSlots > 0 && ! call.args.empty() && literalNumber (call.args[0], slot)
                && (slot != static_cast<double> (static_cast<int> (slot)) || slot < 0.0
                    || slot >= static_cast<double> (e->numSlots)))
                addIssue (r, section, code, call.pos,
                          std::string (e->name) + " slot " + call.args[0] + " is out of range (it has "
                              + plural (e->numSlots, "slot") + ": 0"
                              + (e->numSlots > 1 ? "-" + std::to_string (e->numSlots - 1) : std::string())
                              + ")");
            continue;
        }

        if (const EelBuiltin* b = findBuiltin (call.name))
        {
            const int n = static_cast<int> (call.args.size());
            const bool tooFew = n < b->minArgs;
            const bool tooMany = b->maxArgs >= 0 && n > b->maxArgs;
            if (call.closed && (tooFew || tooMany))
                addIssue (r, section, code, call.pos,
                          std::string (b->name) + " takes "
                              + (b->maxArgs < 0 ? "at least " : "") + plural (b->minArgs, "argument")
                              + ", got " + std::to_string (n));
            continue;
        }

        if (contains (kRefused, call.name))
        {
            addIssue (r, section, code, call.pos, call.name + " is not allowed in Pulsar patches");
            continue;
        }

        if (unknownSeen.insert (call.name).second)
            addIssue (r, section, code, call.pos,
                      "unknown function " + call.name + "; " + suggestFor (call.name));
    }
}

} // namespace

std::string LintResult::message() const
{
    std::string m;
    for (const std::string& i : issues)
        m += (m.empty() ? "" : "; ") + i;
    return m;
}

LintResult lintPatch (const Patch& patch)
{
    // User functions from every section are visible everywhere (EEL2
    // compiles the sections of one VM against the same function table).
    const std::pair<const char*, const std::string*> sections[] = {
        { "init", &patch.init }, { "voice", &patch.voice }, { "master", &patch.master }
    };
    Scan scans[3];
    std::map<std::string, int> functions;
    for (int i = 0; i < 3; ++i)
    {
        scanSection (*sections[i].second, scans[i]);
        functions.insert (scans[i].functions.begin(), scans[i].functions.end());
    }
    LintResult r;
    for (int i = 0; i < 3; ++i)
        checkCalls (*sections[i].second, sections[i].first, scans[i], functions, r);
    return r;
}

LintResult lintSection (const std::string& code, const std::string& section)
{
    Scan scan;
    scanSection (code, scan);
    LintResult r;
    checkCalls (code, section, scan, scan.functions, r);
    return r;
}

int numAllowedBuiltins() noexcept
{
    return kNumBuiltins;
}

const EelBuiltin& allowedBuiltin (int index) noexcept
{
    return kBuiltins[index >= 0 && index < kNumBuiltins ? index : 0];
}

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

unsigned macrosRead (const Patch& patch)
{
    return macrosRead (patch.init) | macrosRead (patch.voice) | macrosRead (patch.master);
}

} // namespace vibesynth
