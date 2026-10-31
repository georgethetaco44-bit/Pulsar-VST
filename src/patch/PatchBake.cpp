// M10: vibesynth_cli --bake. See PatchBake.h.

#include "patch/PatchBake.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <tuple>

#include "engine/NodeMeters.h"
#include "engine/NodeOverrides.h"
#include "patch/LexiconLint.h"
#include "patch/PatchTweaks.h"

namespace vibesynth {

std::string formatEelNumber (double v)
{
    if (! isFiniteBitsD (v) || v == 0.0)
        return "0";
    char buf[64];
    auto roundTrips = [&v] (const char* s) { return std::strtod (s, nullptr) == v; };
    std::string best;
    for (int p = 1; p <= 17; ++p)
    {
        std::snprintf (buf, sizeof (buf), "%.*g", p, v);
        if (roundTrips (buf))
        {
            best = buf;
            break;
        }
    }
    // EEL2's tokenizer has no exponents: fall back to fixed notation.
    if (best.empty() || best.find_first_of ("eE") != std::string::npos)
    {
        best.clear();
        for (int p = 1; p <= 40; ++p)
        {
            std::snprintf (buf, sizeof (buf), "%.*f", p, v);
            if (roundTrips (buf))
            {
                best = buf;
                break;
            }
        }
        if (best.empty())
            best = buf;
    }
    return v < 0.0 ? "(" + best + ")" : best;
}

namespace {

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

std::string trim (const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && isSpace (s[a]))
        ++a;
    while (b > a && isSpace (s[b - 1]))
        --b;
    return s.substr (a, b - a);
}

std::string lower (std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char> (c - 'A' + 'a');
    return s;
}

// A dropped argument that might do something (call or assign) must still
// be evaluated. Over-cautious on purpose (plain grouping counts too).
bool mayHaveSideEffects (const std::string& s)
{
    return s.find ('(') != std::string::npos || s.find ('=') != std::string::npos;
}

struct StatelessCall
{
    int module;
    bool conditional;
};

class SectionBaker
{
public:
    SectionBaker (const std::string& code, RackKind rack, const OverrideTable& table, const std::string& tempPrefix,
                  int& tempCounter)
        : src (code), rack (rack), table (table), tempPrefix (tempPrefix), tempCounter (tempCounter)
    {
    }

    bool run (std::string& out)
    {
        out = rewrite (0, src.size(), false);
        if (! error.empty())
            return false;
        // Stateless numbering must be fixed for every tweaked module.
        for (const auto& c : stateless)
        {
            const bool cond = c.conditional || anyFunction;
            if (! cond)
                continue;
            for (int s = 0; s < kMaxSlotsPerModule; ++s)
                if (table.activeNode ((int) rack, c.module, s) != nullptr)
                {
                    error = std::string (moduleShortName ((ModuleId) c.module)) + " is called inside a branch, loop or "
                            "function here, so its call order (#0, #1, ...) isn't fixed; its tweaks can't be baked";
                    return false;
                }
        }
        return true;
    }

    std::string error;
    std::set<std::tuple<int, int, int>> seen;  // (rack, module, slot)
    int changed = 0;

private:
    // End of a comment or string starting at i, or i if there is none.
    size_t skipLiteral (size_t i, size_t end) const
    {
        if (src[i] == '/' && i + 1 < end && src[i + 1] == '/')
        {
            while (i < end && src[i] != '\n')
                ++i;
            return i;
        }
        if (src[i] == '/' && i + 1 < end && src[i + 1] == '*')
        {
            const size_t close = src.find ("*/", i + 2);
            return close == std::string::npos || close + 2 > end ? end : close + 2;
        }
        if (src[i] == '"' || src[i] == '\'')
        {
            const char q = src[i];
            ++i;
            while (i < end && src[i] != q)
                i += src[i] == '\\' ? 2 : 1;
            return i < end ? i + 1 : end;
        }
        return i;
    }

    size_t matchParen (size_t open) const
    {
        int depth = 0;
        for (size_t i = open; i < src.size();)
        {
            const size_t lit = skipLiteral (i, src.size());
            if (lit != i)
            {
                i = lit;
                continue;
            }
            if (src[i] == '(')
                ++depth;
            else if (src[i] == ')' && --depth == 0)
                return i;
            ++i;
        }
        return std::string::npos;
    }

    // Argument ranges of the call whose parens are at [open, close].
    std::vector<std::pair<size_t, size_t>> splitArgs (size_t open, size_t close) const
    {
        std::vector<std::pair<size_t, size_t>> args;
        size_t start = open + 1;
        int depth = 0;
        for (size_t i = open + 1; i < close;)
        {
            const size_t lit = skipLiteral (i, close);
            if (lit != i)
            {
                i = lit;
                continue;
            }
            if (src[i] == '(')
                ++depth;
            else if (src[i] == ')')
                --depth;
            else if (src[i] == ',' && depth == 0)
            {
                args.emplace_back (start, i);
                start = i + 1;
            }
            ++i;
        }
        if (! trim (src.substr (start, close - start)).empty() || ! args.empty())
            args.emplace_back (start, close);
        return args;
    }

    std::string rewrite (size_t b, size_t e, bool inherited)
    {
        std::string out;
        bool cond = inherited;
        size_t i = b;
        while (i < e && error.empty())
        {
            const size_t lit = skipLiteral (i, e);
            if (lit != i)
            {
                out += src.substr (i, lit - i);
                i = lit;
                continue;
            }
            const char c = src[i];
            if (c == ';')
            {
                cond = inherited;
                out += c;
                ++i;
                continue;
            }
            if (c == '?' || ((c == '&' || c == '|') && i + 1 < e && src[i + 1] == c))
                cond = true;
            if (c == '(')
            {
                const size_t close = matchParen (i);
                if (close == std::string::npos || close >= e)
                {
                    error = "unbalanced parentheses";
                    return out;
                }
                out += "(" + rewrite (i + 1, close, cond) + ")";
                i = close + 1;
                continue;
            }
            if (c >= '0' && c <= '9')
            {
                size_t j = i;
                while (j < e && (isIdentChar (src[j])))
                    ++j;
                out += src.substr (i, j - i);
                i = j;
                continue;
            }
            if (isIdentStart (c))
            {
                size_t j = i;
                while (j < e && isIdentChar (src[j]))
                    ++j;
                const std::string name = src.substr (i, j - i);
                if (lower (name) == "function")
                    anyFunction = true;
                size_t k = j;
                while (k < e && isSpace (src[k]))
                    ++k;
                if (k < e && src[k] == '(')
                {
                    const size_t close = matchParen (k);
                    if (close == std::string::npos || close >= e)
                    {
                        error = "unbalanced parentheses";
                        return out;
                    }
                    if (const LexiconEntry* entry = findLexiconEntry (name.c_str()))
                    {
                        out += bakeCall (*entry, i, k, close, cond);
                    }
                    else
                    {
                        const std::string ln = lower (name);
                        const bool loop = ln == "loop" || ln == "while";
                        out += src.substr (i, k + 1 - i) + rewrite (k + 1, close, cond || loop) + ")";
                    }
                    i = close + 1;
                    continue;
                }
                out += name;
                i = j;
                continue;
            }
            out += c;
            ++i;
        }
        return out;
    }

    std::string newTemp() { return tempPrefix + std::to_string (++tempCounter); }

    std::string bakeCall (const LexiconEntry& e, size_t nameStart, size_t open, size_t close, bool cond)
    {
        const auto ranges = splitArgs (open, close);
        std::vector<std::string> args;
        for (const auto& r : ranges)
            args.push_back (rewrite (r.first, r.second, cond));
        auto rebuild = [&] (const std::vector<std::string>& a)
        {
            std::string s = src.substr (nameStart, open + 1 - nameStart);
            for (size_t i = 0; i < a.size(); ++i)
                s += (i > 0 ? "," : "") + a[i];
            return s + ")";
        };
        const int m = e.module;
        if (m < 0 || (int) args.size() != e.numArgs)
            return rebuild (args);

        int slot = 0;
        if (e.numSlots > 0)
        {
            const std::string lit = trim (src.substr (ranges[0].first, ranges[0].second - ranges[0].first));
            char* end = nullptr;
            const double v = std::strtod (lit.c_str(), &end);
            const bool literal = ! lit.empty() && end != nullptr && *end == 0;
            if (! literal)
            {
                for (int s = 0; s < kMaxSlotsPerModule; ++s)
                    if (table.activeNode ((int) rack, m, s) != nullptr)
                    {
                        error = std::string (e.name) + " has a slot that isn't a number (" + lit + "), so its tweaks "
                                "can't be matched to it";
                        return rebuild (args);
                    }
                return rebuild (args);
            }
            slot = clampSlot (v, e.numSlots);
        }
        else if (isStatelessModule ((ModuleId) m))
        {
            slot = counts[m] < kMaxSlotsPerModule - 1 ? counts[m] : kMaxSlotsPerModule - 1;
            ++counts[m];
            stateless.push_back ({ m, cond });
        }
        seen.insert ({ (int) rack, m, slot });

        const NodeOverride* node = table.activeNode ((int) rack, m, slot);
        if (node == nullptr)
            return rebuild (args);

        const bool bypass = e.bypass != BypassKind::None && node->bypass.load() != 0;
        if (bypass && (e.bypass == BypassKind::PassArg || e.bypass == BypassKind::Zero))
        {
            ++changed;
            const bool pass = e.bypass == BypassKind::PassArg;
            const int k = e.bypassArg;
            bool anySide = false;
            for (int i = 0; i < e.numArgs; ++i)
                if ((! pass || i != k) && mayHaveSideEffects (args[(size_t) i]))
                    anySide = true;
            if (! anySide)
                return pass ? "(" + trim (args[(size_t) k]) + ")" : std::string ("0");
            // Keep every argument that may do something, in order.
            const std::string t = pass ? newTemp() : std::string();
            std::string s = "(";
            for (int i = 0; i < e.numArgs; ++i)
            {
                if (pass && i == k)
                    s += t + " = (" + args[(size_t) i] + "); ";
                else if (mayHaveSideEffects (args[(size_t) i]))
                    s += "(" + args[(size_t) i] + "); ";
            }
            return s + (pass ? t : std::string ("0")) + ")";
        }

        std::vector<std::string> out = args;
        if (e.primary)
            for (int i = 0; i < e.numArgs; ++i)
            {
                const LexiconArg& meta = e.args[i];
                const auto& ov = node->args[i];
                const auto mode = static_cast<ArgMode> (ov.mode.load());
                if (mode == ArgMode::None || ! isEditableArg (meta.kind))
                    continue;
                const std::string a = trim (args[(size_t) i]);
                const double applied = ov.applied.load();
                if (mode == ArgMode::Offset && meta.kind == ArgKind::Hz)
                    out[(size_t) i] = "((" + a + ") * " + formatEelNumber (applied) + ")";
                else if (mode == ArgMode::Offset)
                    out[(size_t) i] = "min(max((" + a + ") + " + formatEelNumber (applied) + ", "
                                      + formatEelNumber (meta.lo) + "), " + formatEelNumber (meta.hi) + ")";
                else if (mode == ArgMode::Replace)
                    out[(size_t) i] = mayHaveSideEffects (a) ? "((" + a + "); " + formatEelNumber (applied) + ")"
                                                             : formatEelNumber (applied);
                else
                {
                    const double lo = ov.mapLo.load();
                    const double span = ov.mapHi.load() - lo;
                    const std::string expr = formatEelNumber (lo) + " + m" + std::to_string ((ov.macro.load() & 7) + 1)
                                           + " * " + formatEelNumber (span);
                    out[(size_t) i] = mayHaveSideEffects (a) ? "((" + a + "); " + expr + ")" : "(" + expr + ")";
                }
            }
        if (bypass && e.bypass == BypassKind::ForceArg && e.bypassArg >= 0 && e.bypassArg < e.numArgs)
        {
            const std::string a = trim (out[(size_t) e.bypassArg]);
            const std::string v = formatEelNumber (e.bypassValue);
            out[(size_t) e.bypassArg] = mayHaveSideEffects (a) ? "((" + a + "); " + v + ")" : v;
        }
        if (out != args)
            ++changed;
        // Keep each argument's own spacing around a rewritten expression.
        for (size_t i = 0; i < out.size(); ++i)
            if (out[i] != args[i])
            {
                const std::string& a = args[i];
                size_t lead = 0, tail = a.size();
                while (lead < tail && isSpace (a[lead]))
                    ++lead;
                while (tail > lead && isSpace (a[tail - 1]))
                    --tail;
                out[i] = a.substr (0, lead) + trim (out[i]) + a.substr (tail);
            }
        return rebuild (out);
    }

    const std::string& src;
    RackKind rack;
    const OverrideTable& table;
    std::string tempPrefix;
    int& tempCounter;
    int counts[kNumModules] = {};
    std::vector<StatelessCall> stateless;
    bool anyFunction = false;
};

} // namespace

BakeResult bakePatch (const Patch& in)
{
    BakeResult r;
    r.patch = in;
    r.patch.tweaks.clear();
    r.patch.macroMaps.clear();

    // The engine's own resolution (names, ranges, applied doubles).
    auto table = std::make_unique<OverrideTable>();
    applyPatchTweaks (in, *table, false);
    if (! in.mods.empty())
        r.notes.push_back ("mods left untouched (they aren't code)");

    std::string prefix = "bake_t";
    while (in.init.find (prefix) != std::string::npos || in.voice.find (prefix) != std::string::npos
           || in.master.find (prefix) != std::string::npos)
        prefix = "_" + prefix;
    int temps = 0;

    std::set<std::tuple<int, int, int>> seen;
    struct Section
    {
        const char* name;
        const std::string* code;
        std::string* out;
        RackKind rack;
    };
    const Section sections[] = { { "init", &in.init, &r.patch.init, RackKind::Voice },
                                 { "voice", &in.voice, &r.patch.voice, RackKind::Voice },
                                 { "master", &in.master, &r.patch.master, RackKind::Master } };
    for (const auto& s : sections)
    {
        SectionBaker baker (*s.code, s.rack, *table, prefix, temps);
        std::string baked;
        if (! baker.run (baked))
        {
            r.error = std::string (s.name) + " code: " + baker.error;
            return r;
        }
        if (baked.size() > Patch::kMaxSectionBytes)
        {
            r.error = std::string (s.name) + " code would be too large after baking";
            return r;
        }
        *s.out = baked;
        r.callsChanged += baker.changed;
        seen.insert (baker.seen.begin(), baker.seen.end());
    }

    // Tweaks the code has no call for are dropped (and said so).
    for (int rk = 0; rk < kNumRackKinds; ++rk)
        for (int m = 0; m < kNumModules; ++m)
            for (int s = 0; s < kMaxSlotsPerModule; ++s)
                if (table->activeNode (rk, m, s) != nullptr && seen.count ({ rk, m, s }) == 0)
                    r.notes.push_back ("no call for " + nodeIdString ({ (RackKind) rk, (ModuleId) m, s })
                                       + "; its tweaks were dropped");

    // An unused macro that gained a map gets the argument's name.
    const unsigned read = macrosRead (in);
    for (const auto& map : in.macroMaps)
    {
        const int i = map.macro - 1;
        NodeRef n;
        if (i < 0 || i >= Patch::kMaxMacros || (read & (1u << i)) != 0u || ! parseNodeId (map.node, n))
            continue;
        const LexiconEntry* e = primaryLexiconEntry ((int) n.module);
        const int k = e != nullptr ? lexiconArgIndex (*e, map.arg.c_str()) : -1;
        if (k < 0)
            continue;
        while ((int) r.patch.macros.size() <= i)
            r.patch.macros.push_back ({});
        if (r.patch.macros[(size_t) i].label.empty())
        {
            r.patch.macros[(size_t) i].label = e->args[k].label;
            r.notes.push_back ("macro " + std::to_string (map.macro) + " labelled \"" + e->args[k].label + "\"");
        }
    }
    r.ok = true;
    return r;
}

} // namespace vibesynth
