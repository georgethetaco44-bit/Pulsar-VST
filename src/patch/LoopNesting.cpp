// Loop-nesting check (SPEC §7.1). See LoopNesting.h for the rules.

#include "patch/LoopNesting.h"

#include <algorithm>
#include <map>
#include <vector>

namespace vibesynth {

namespace {

// Any user function that (indirectly) calls itself: always too deep.
constexpr int kRecursionDepth = 99;

struct Token
{
    enum class Kind { Ident, Open, Close };
    Kind kind;
    std::string text; // lower-case identifier (Ident only)
};

using Tokens = std::vector<Token>;

// Comments, "strings" and 'char' constants become spaces, so nothing
// inside them can open, close or name a loop.
std::string blankCommentsAndStrings (const std::string& code)
{
    std::string s = code;
    const size_t n = s.size();
    size_t i = 0;
    while (i < n)
    {
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '/')
        {
            while (i < n && s[i] != '\n')
                s[i++] = ' ';
        }
        else if (s[i] == '/' && i + 1 < n && s[i + 1] == '*')
        {
            s[i++] = ' ';
            s[i++] = ' ';
            while (i < n && ! (s[i] == '*' && i + 1 < n && s[i + 1] == '/'))
                s[i++] = ' ';
            for (int k = 0; k < 2 && i < n; ++k)
                s[i++] = ' ';
        }
        else if (s[i] == '"' || s[i] == '\'')
        {
            const char quote = s[i];
            s[i++] = ' ';
            while (i < n && s[i] != quote)
            {
                if (s[i] == '\\' && i + 1 < n)
                    s[i++] = ' ';
                s[i++] = ' ';
            }
            if (i < n)
                s[i++] = ' ';
        }
        else
        {
            ++i;
        }
    }
    return s;
}

bool isIdentChar (char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
        || c == '_' || c == '.' || c == '#' || c == '$';
}

Tokens tokenize (const std::string& code)
{
    const std::string s = blankCommentsAndStrings (code);
    Tokens out;
    size_t i = 0;
    while (i < s.size())
    {
        const char c = s[i];
        if (c == '(')
        {
            out.push_back ({ Token::Kind::Open, {} });
            ++i;
        }
        else if (c == ')')
        {
            out.push_back ({ Token::Kind::Close, {} });
            ++i;
        }
        else if (isIdentChar (c))
        {
            std::string id;
            while (i < s.size() && isIdentChar (s[i]))
            {
                const char ch = s[i++];
                id += (ch >= 'A' && ch <= 'Z') ? static_cast<char> (ch - 'A' + 'a') : ch;
            }
            out.push_back ({ Token::Kind::Ident, id });
        }
        else
        {
            ++i;
        }
    }
    return out;
}

// "this.foo" and "foo" call the same function as far as nesting goes.
std::string baseName (const std::string& id)
{
    const size_t dot = id.rfind ('.');
    return dot == std::string::npos ? id : id.substr (dot + 1);
}

// Index of the Close matching the Open at `open` (or tokens.size()).
size_t matchingClose (const Tokens& t, size_t open)
{
    int level = 0;
    for (size_t i = open; i < t.size(); ++i)
    {
        if (t[i].kind == Token::Kind::Open)
            ++level;
        else if (t[i].kind == Token::Kind::Close && --level == 0)
            return i;
    }
    return t.size();
}

struct FunctionBody
{
    const Tokens* tokens;
    size_t begin; // the body's Open
    size_t end;   // its matching Close (exclusive bound for the scan)
};

// function name(params) [local(...) instance(...) ...] ( body )
void collectFunctions (const Tokens& t, std::multimap<std::string, FunctionBody>& out)
{
    for (size_t i = 0; i + 2 < t.size(); ++i)
    {
        if (t[i].kind != Token::Kind::Ident || t[i].text != "function"
            || t[i + 1].kind != Token::Kind::Ident || t[i + 2].kind != Token::Kind::Open)
            continue;
        size_t j = matchingClose (t, i + 2) + 1; // past the parameter list
        while (j + 1 < t.size() && t[j].kind == Token::Kind::Ident && t[j + 1].kind == Token::Kind::Open)
            j = matchingClose (t, j + 1) + 1;    // local(...), instance(...), ...
        if (j < t.size() && t[j].kind == Token::Kind::Open)
            out.insert ({ baseName (t[i + 1].text), { &t, j, matchingClose (t, j) } });
    }
}

class NestingScanner
{
public:
    explicit NestingScanner (const std::multimap<std::string, FunctionBody>& f) : functions (f) {}

    // Deepest nesting inside t[begin, end), starting from level 0.
    int scan (const Tokens& t, size_t begin, size_t end)
    {
        struct Frame { bool loop; bool whileCond; };
        std::vector<Frame> stack;
        int depth = 0;
        int deepest = 0;
        bool nextIsLoop = false;
        bool nextIsWhile = false;
        bool whileBody = false; // the previous token closed a while(...)

        for (size_t i = begin; i < end && i < t.size(); ++i)
        {
            const Token& tok = t[i];
            if (tok.kind == Token::Kind::Ident)
            {
                whileBody = false;
                const bool call = i + 1 < t.size() && t[i + 1].kind == Token::Kind::Open;
                if (! call)
                    continue;
                if (tok.text == "loop" || tok.text == "while")
                {
                    nextIsLoop = true;
                    nextIsWhile = tok.text == "while";
                }
                else if (functions.count (baseName (tok.text)) > 0)
                {
                    deepest = std::max (deepest, depth + functionDepth (baseName (tok.text)));
                }
            }
            else if (tok.kind == Token::Kind::Open)
            {
                const bool loop = nextIsLoop || whileBody;
                stack.push_back ({ loop, nextIsLoop && nextIsWhile });
                if (loop)
                    deepest = std::max (deepest, ++depth);
                nextIsLoop = nextIsWhile = whileBody = false;
            }
            else
            {
                whileBody = false;
                if (stack.empty())
                    continue; // unbalanced: EEL2 rejects it at compile
                const Frame f = stack.back();
                stack.pop_back();
                if (f.loop)
                    --depth;
                whileBody = f.whileCond;
            }
        }
        return deepest;
    }

private:
    int functionDepth (const std::string& name)
    {
        const auto known = memo.find (name);
        if (known != memo.end())
            return known->second;
        if (std::find (visiting.begin(), visiting.end(), name) != visiting.end())
            return kRecursionDepth;
        visiting.push_back (name);
        int d = 0;
        const auto range = functions.equal_range (name);
        for (auto it = range.first; it != range.second; ++it)
            d = std::max (d, scan (*it->second.tokens, it->second.begin, it->second.end + 1));
        visiting.pop_back();
        memo[name] = d;
        return d;
    }

    const std::multimap<std::string, FunctionBody>& functions;
    std::map<std::string, int> memo;
    std::vector<std::string> visiting;
};

} // namespace

LoopNestingResult checkLoopNesting (const Patch& patch)
{
    const char* names[] = { "init", "voice", "master" };
    const Tokens sections[] = { tokenize (patch.init), tokenize (patch.voice), tokenize (patch.master) };
    std::multimap<std::string, FunctionBody> functions;
    for (const Tokens& t : sections)
        collectFunctions (t, functions);

    NestingScanner scanner (functions);
    LoopNestingResult result;
    for (int s = 0; s < 3; ++s)
    {
        const int d = scanner.scan (sections[s], 0, sections[s].size());
        if (d > result.depth)
        {
            result.depth = d;
            result.section = names[s];
        }
    }
    return result;
}

int loopNestingDepth (const std::string& code)
{
    const Tokens t = tokenize (code);
    std::multimap<std::string, FunctionBody> functions;
    collectFunctions (t, functions);
    NestingScanner scanner (functions);
    return scanner.scan (t, 0, t.size());
}

std::string loopNestingError (const LoopNestingResult& r)
{
    if (r.ok())
        return {};
    return "loops nested too deep (max " + std::to_string (kMaxLoopNesting) + ") in " + r.section;
}

} // namespace vibesynth
