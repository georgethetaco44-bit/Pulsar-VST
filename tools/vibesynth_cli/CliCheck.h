#pragma once

// vibesynth_cli test line: "[PASS] ..." / "[FAIL] ..." (tools/check.sh
// filters on these prefixes). Returns `ok`.

#include <cstdarg>
#include <cstdio>

namespace vibesynth_cli {

inline bool check (bool ok, const char* fmt, ...)
{
    std::printf ("[%s] ", ok ? "PASS" : "FAIL");
    va_list args;
    va_start (args, fmt);
    std::vprintf (fmt, args);
    va_end (args);
    std::printf ("\n");
    return ok;
}

} // namespace vibesynth_cli
