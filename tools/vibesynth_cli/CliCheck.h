#pragma once

// vibesynth_cli test line: "[PASS] ..." / "[FAIL] ..." (tools/check.sh
// filters on these prefixes). Returns `ok`. While a CheckPrefix is alive,
// its text starts every line (a test group run twice, e.g. on CRLF input).

#include <cstdarg>
#include <cstdio>

namespace vibesynth_cli {

inline const char*& checkPrefix()
{
    static const char* prefix = "";
    return prefix;
}

struct CheckPrefix
{
    explicit CheckPrefix (const char* p) : previous (checkPrefix()) { checkPrefix() = p; }
    ~CheckPrefix() { checkPrefix() = previous; }
    CheckPrefix (const CheckPrefix&) = delete;
    CheckPrefix& operator= (const CheckPrefix&) = delete;

    const char* previous;
};

inline bool check (bool ok, const char* fmt, ...)
{
    std::printf ("[%s] %s", ok ? "PASS" : "FAIL", checkPrefix());
    va_list args;
    va_start (args, fmt);
    std::vprintf (fmt, args);
    va_end (args);
    std::printf ("\n");
    return ok;
}

} // namespace vibesynth_cli
