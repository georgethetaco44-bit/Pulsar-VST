cmake_minimum_required(VERSION 3.25)

# Static MSVC runtime on Windows (SPEC 10.1). No-op on other platforms.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Universal binary + deployment target on macOS (SPEC 10.1). Must be set
# before project() as cache variables so they apply to everything.
# No-op on other platforms.
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "macOS architectures")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "macOS deployment target")

project(VibeSynthVST VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for clangd.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Use ccache when available (SPEC 10.1).
find_program(CCACHE_PROGRAM ccache)
if (CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# Controls whether built plugins are copied into system plugin folders
# (e.g. ~/.vst3). ON locally so REAPER picks up the build; CI passes
# -DVIBESYNTH_COPY_PLUGIN=OFF so runners stay clean.
option(VIBESYNTH_COPY_PLUGIN "Copy built plugins to system plugin folders" ON)

include(cmake/CPM.cmake)

# Threads needed by EEL2 (worker mutexes around RAM alloc / tables).
find_package(Threads REQUIRED)

# EEL2 static library (SPEC §4/§10.1; see cmake/EEL2.cmake).
include(cmake/EEL2.cmake)

# M4 — DaisySP (MIT repo only, pinned by SHA; SPEC §5.2, §10.4, §11)
#
# Licence: MIT (electro-smith/DaisySP). Its .gitmodules pulls DaisySP-LGPL
# as a submodule — and CPM 0.42.1 does NOT forward GIT_SUBMODULES through
# cpm_add_package/CPMAddPackage into FetchContent_Declare, so passing it is
# useless (verified: the submodule was still cloned). Instead we fetch with
# DOWNLOAD_ONLY and build our own `daisysp_mit` static lib from an explicit
# source whitelist below, so the LGPL tree can never enter the build.
# Configure still FAILS if anything LGPL is present.
set(VIBESYNTH_DAISYSP_PIN 2c72eaf9eac5fc0dca1919d65d606da907832618)
CPMAddPackage(
    NAME DaisySP
    GITHUB_REPOSITORY electro-smith/DaisySP
    GIT_TAG ${VIBESYNTH_DAISYSP_PIN}
    GIT_SHALLOW FALSE
    DOWNLOAD_ONLY YES
)

# Guard: the two LGPL-only classes must be absent from every source file we
# compile. DaisySP-LGPL is a registered submodule of the MIT repo, so its
# directory may exist inertly in the CPM cache — that is harmless because
# only the whitelisted MIT files below are ever added to a build target.
# What configure forbids is any LGPL *content* entering the build.
foreach(_lgpl_file Source/Filters/moogladder.h Source/Effects/reverbsc.h)
    if (EXISTS "${DaisySP_SOURCE_DIR}/${_lgpl_file}")
        message(FATAL_ERROR "VibeSynth: ${_lgpl_file} must come from the "
                "LGPL repo, not the MIT pin — refusing to use it.")
    endif()
endforeach()
# Repo rule 10: the DaisySP include root (Source/) must hold no .cpp files,
# so it is safe to put on an include path. All module .cpp live in subdirs.
file(GLOB _daisy_root_cpp "${DaisySP_SOURCE_DIR}/Source/*.cpp")
if (_daisy_root_cpp)
    message(FATAL_ERROR "VibeSynth: unexpected .cpp at DaisySP Source root: "
            "${_daisy_root_cpp} (repo rule 10).")
endif()

# Our own static lib from an explicit whitelist of MIT-repo sources only.
# (KarplusString.cpp also needs Dynamics/crossfade.cpp and Utility/dcblock.cpp;
#  DelayLine/onepole are header-only.)
set(VIBESYNTH_DAISYSP_SOURCES
    "${DaisySP_SOURCE_DIR}/Source/Synthesis/oscillator.cpp"
    "${DaisySP_SOURCE_DIR}/Source/Filters/svf.cpp"
    "${DaisySP_SOURCE_DIR}/Source/Filters/ladder.cpp"
    "${DaisySP_SOURCE_DIR}/Source/Control/adsr.cpp"
    "${DaisySP_SOURCE_DIR}/Source/Effects/decimator.cpp"
    "${DaisySP_SOURCE_DIR}/Source/Effects/chorus.cpp"
    "${DaisySP_SOURCE_DIR}/Source/PhysicalModeling/resonator.cpp"
    "${DaisySP_SOURCE_DIR}/Source/PhysicalModeling/KarplusString.cpp"
    "${DaisySP_SOURCE_DIR}/Source/Dynamics/crossfade.cpp"
    "${DaisySP_SOURCE_DIR}/Source/Utility/dcblock.cpp"
)
add_library(daisysp_mit STATIC ${VIBESYNTH_DAISYSP_SOURCES})
# The .cpp files #include sibling-module headers bare ("svf.h", "dsp.h"):
# upstream puts every module dir on its own private include path. Mirror
# that, but keep PRIVATE (repo rule 10 applies to our own targets' paths —
# Daisy module dirs are upstream's layout, not ours, and this stays local
# to the daisysp_mit target).
target_include_directories(daisysp_mit
    PUBLIC "${DaisySP_SOURCE_DIR}/Source"
    PRIVATE
        "${DaisySP_SOURCE_DIR}/Source/Utility"
        "${DaisySP_SOURCE_DIR}/Source/Control"
        "${DaisySP_SOURCE_DIR}/Source/Drums"
        "${DaisySP_SOURCE_DIR}/Source/Dynamics"
        "${DaisySP_SOURCE_DIR}/Source/Effects"
        "${DaisySP_SOURCE_DIR}/Source/Filters"
        "${DaisySP_SOURCE_DIR}/Source/Noise"
        "${DaisySP_SOURCE_DIR}/Source/PhysicalModeling"
        "${DaisySP_SOURCE_DIR}/Source/Sampling"
        "${DaisySP_SOURCE_DIR}/Source/Synthesis")
# Upstream builds these as C++14; keep that here so behaviour matches Daisy.
set_target_properties(daisysp_mit PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)
# -O3 for engine code (SPEC §11); never fast-math anywhere (it would let the
# compiler delete NaN/Inf checks in our wrappers and safety code).
target_compile_options(daisysp_mit PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-O3;-fno-fast-math>
    $<$<CXX_COMPILER_ID:MSVC>:/O2>)

# JUCE 8 pinned to an exact release tag (SPEC 10.1).
CPMAddPackage(
    NAME JUCE
    GITHUB_REPOSITORY juce-framework/JUCE
    GIT_TAG 8.0.15
)

# libcurl networking backend on Linux only (SPEC 10.1). JUCE's own CMake
# defines JUCE_USE_CURL=1 and links curl when NEEDS_CURL is set, and sets
# JUCE_WEB_BROWSER=0 when NEEDS_WEB_BROWSER is unset.
set(VIBESYNTH_LINUX_OPTS "")
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(VIBESYNTH_LINUX_OPTS NEEDS_CURL TRUE)
endif()

juce_add_plugin(VibeSynthVST
    VERSION 0.1.0
    COMPANY_NAME "Pulsar"
    BUNDLE_ID com.pulsar.vibesynthvst
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${VIBESYNTH_COPY_PLUGIN}
    PLUGIN_MANUFACTURER_CODE Puls
    PLUGIN_CODE Vsyn
    FORMATS VST3 Standalone
    PRODUCT_NAME "Pulsar"
    PLUGIN_NAME "Pulsar"
    ${VIBESYNTH_LINUX_OPTS})

target_sources(VibeSynthVST PRIVATE
    src/PluginProcessor.cpp)

target_include_directories(VibeSynthVST PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(VibeSynthVST PUBLIC
    JUCE_VST3_CAN_REPLACE_VST2=0)

# The plugin runs the shared engine (eel2 + DaisySP lexicon + PatchEngine);
# see the vibesynth_engine target below. No engine .cpp here directly.
target_link_libraries(VibeSynthVST PRIVATE
    vibesynth_engine
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# M4: the shared DSP engine, linked into the plugin and the CLI alike
# (SPEC §10.3) so both run exactly the same code. Lexicon + PatchEngine,
# backed by eel2 and DaisySP (MIT pin above).
add_library(vibesynth_engine STATIC
    src/patch/PatchEngine.cpp
    src/engine/LexiconRacks.cpp
    src/engine/Racks.cpp
    src/engine/Racks2.cpp
    src/engine/LexiconFn1.cpp
    src/engine/LexiconFn2.cpp
    src/engine/LexiconFn3.cpp
    src/engine/LexiconFn4.cpp
    src/engine/LexiconFn5.cpp
    src/engine/LexiconFn6.cpp
    src/engine/LexiconReg.cpp)
target_include_directories(vibesynth_engine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/WDL/WDL")
target_link_libraries(vibesynth_engine PUBLIC eel2 daisysp_mit)
# Linked into the VST3 shared module, so it must be position independent
# (same as eel2 and daisysp_mit).
set_target_properties(vibesynth_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
# -O3 for engine code (SPEC §11); never fast-math (see daisysp_mit above).
target_compile_options(vibesynth_engine PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-O3;-fno-fast-math>
    $<$<CXX_COMPILER_ID:MSVC>:/O2>)

# Headless test tool (SPEC §10.3). Console target sharing the engine code;
# CI runs --smoketest (gating); --bench is report-only. Engine headers come
# through vibesynth_engine's public include dirs (no repo-root include path,
# repo rule 10).
add_executable(vibesynth_cli
    tools/vibesynth_cli/main.cpp
    tools/vibesynth_cli/Bench.cpp)
target_link_libraries(vibesynth_cli PRIVATE vibesynth_engine)
