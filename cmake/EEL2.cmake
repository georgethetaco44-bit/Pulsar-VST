# Milestone 3 — EEL2 (Milestone 2 rules carry over: SPEC is source of truth,
# one milestone at a time, Distrobox `juce` builds, no audio-thread
# allocation/locks/logging/NaN-`std::isnan`, permissive deps only,
# Pulsar externally / VibeSynth in code.)

# Builds the vendored EEL2 subset in third_party/WDL as a static library.
# See third_party/WDL/VENDORED.md for the WDL pin and exactly which files
# are vendored. Mirrors ysfx's approach: one static lib, portable-mode
# switch, Windows-x64 prebuilt asm object, GAS .S for gcc/clang (including
# the macOS Universal slices — the arch selection is all preprocessor
# macros, so a single Universal compile works).
option(VIBESYNTH_EEL_PORTABLE "Use EEL2 portable (non-JIT) bytecode mode" OFF)

set(VIBESYNTH_WDL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/WDL/WDL")
set(VIBESYNTH_EEL2_DIR "${VIBESYNTH_WDL_DIR}/eel2")

set(VIBESYNTH_EEL2_SOURCES
    "${VIBESYNTH_EEL2_DIR}/nseel-caltab.c"
    "${VIBESYNTH_EEL2_DIR}/nseel-cfunc.c"
    "${VIBESYNTH_EEL2_DIR}/nseel-compiler.c"
    "${VIBESYNTH_EEL2_DIR}/nseel-eval.c"
    "${VIBESYNTH_EEL2_DIR}/nseel-lextab.c"
    "${VIBESYNTH_EEL2_DIR}/nseel-ram.c"
    "${VIBESYNTH_EEL2_DIR}/nseel-yylex.c")

add_library(eel2 STATIC)
target_sources(eel2 PRIVATE ${VIBESYNTH_EEL2_SOURCES})

# EEL2 is linked into the plugin shared objects: it must be position
# independent on every platform (ysfx sets CMAKE_POSITION_INDEPENDENT_CODE
# globally for the same reason).
set_target_properties(eel2 PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Notes and licence files must NEVER live in a directory that is on an
# include path, and no vendored filename may match a standard header
# (version, limits, string, ...): macOS and Windows filesystems are
# case-insensitive, so e.g. a "VERSION" notes file shadows libc++'s
# <version>. The include root here is therefore third_party/WDL/WDL (headers
# only); third_party/WDL itself holds VENDORED.md and is not on any path.
# Consumers write #include "eel2/ns-eel.h".
target_include_directories(eel2 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/WDL/WDL")

target_link_libraries(eel2 PRIVATE Threads::Threads)

# M5 runaway guard (SPEC §7.1): one loop()/while() runs at most 1024
# iterations (WDL default 1,048,576). ns-eel.h guards the macro with
# #ifndef. PUBLIC so every TU that includes ns-eel.h agrees on the value.
# The cap is emitted by the C glue headers compiled into nseel-compiler.c
# (glue_x86_64_sse.h, glue_aarch64.h, glue_port.h); the prebuilt Windows
# x64 asm object holds only math helpers, so it applies there too.
# vibesynth_cli --smoketest verifies it on every backend.
target_compile_definitions(eel2 PUBLIC NSEEL_LOOPFUNC_SUPPORT_MAXLEN=1024)

if (VIBESYNTH_EEL_PORTABLE)
    # EEL2 bytecode interpreter: no executable pages, ~1/8 JIT speed.
    target_compile_definitions(eel2 PUBLIC EEL_TARGET_PORTABLE)
else()
    if (MSVC)
        # MSVC Win64: WDL's shipped prebuilt object (from WDL's
        # regenerate-x64-objects.sh, `nasm -f win64`). No NASM step needed.
        # Other MSVC arches fall back: aarch64/arm64ec haves no prebuilt
        # object here — portable mode must be used for those slices.
        if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$" OR MSVC_C_ARCHITECTURE_ID STREQUAL "x64")
            target_sources(eel2 PRIVATE "${VIBESYNTH_EEL2_DIR}/asm-nseel-x64.obj")
        else()
            message(WARNING "VibeSynth: no prebuilt EEL2 asm for this MSVC arch; "
                            "EEL2 JIT glue will be missing. Consider -DVIBESYNTH_EEL_PORTABLE=ON.")
        endif()
    else()
        # gcc/clang (Linux, macOS incl. Universal): ysfx's GAS conversion.
        # The file self-guards to __x86_64__ (empty on arm64, which uses
        # WDL's C glue via asm-nseel-aarch64-gcc.c), and handles non-Apple
        # vs Apple symbol mangling, so one Universal compile just works.
        enable_language(ASM)
        set_source_files_properties(
            "${VIBESYNTH_EEL2_DIR}/asm-nseel-x64-sse.S" PROPERTIES LANGUAGE ASM)
        target_sources(eel2 PRIVATE "${VIBESYNTH_EEL2_DIR}/asm-nseel-x64-sse.S")
    endif()
endif()

# Matches WDL's own Makefile / ysfx flags for this subset.
if (NOT WIN32)
    target_compile_definitions(eel2 PRIVATE _FILE_OFFSET_BITS=64)
endif()
if (MSVC)
    target_compile_definitions(eel2 PRIVATE _CRT_NONSTDC_NO_WARNINGS)
else()
    # wdltypes.h wants signed char; ARM targets default to unsigned.
    target_compile_options(eel2 PUBLIC -fsigned-char)
endif()

# wdltypes.h includes <windows.h>; keep the min/max macros out of EEL2's
# own TUs too (vibesynth_engine sets NOMINMAX PUBLIC for everything else).
target_compile_definitions(eel2 PRIVATE $<$<PLATFORM_ID:Windows>:NOMINMAX>)

# Never fast-math on this target: bit-pattern NaN/Inf checks (SPEC §11)
# must survive compiler flags anywhere EEL2 output is validated.
target_compile_options(eel2 PRIVATE $<$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>>:-fno-fast-math>)
