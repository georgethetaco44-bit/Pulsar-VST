// MSVC compatibility shim for the daisysp_mit target only. Force-included
// (/FI, see CMakeLists.txt); never on an include path.
//
// DaisySP's Filters/ladder.cpp marks a function with the GCC-only
// __attribute__((optimize("unroll-loops"))). MSVC has no __attribute__, so
// it is defined away here; the hint only affects GCC's loop unrolling.
#pragma once

#ifdef _MSC_VER
#define __attribute__(x)
#endif
