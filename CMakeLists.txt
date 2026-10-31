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
# MSVC only: Control/adsr.cpp uses M_E (needs _USE_MATH_DEFINES). (M7
# dropped Filters/ladder.cpp and with it the __attribute__ compat header.)
target_compile_definitions(daisysp_mit PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:_USE_MATH_DEFINES>)

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

# M5: the .pulsar model (parse/serialize, built-in preset list). It uses
# juce::JSON, so it can't go into vibesynth_engine: JUCE modules are
# INTERFACE targets that compile their sources into every consumer, and
# linking juce_core into the engine as well as the plugin would duplicate
# every JUCE symbol. The plugin and vibesynth_cli each compile this list.
set(VIBESYNTH_PATCH_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/patch/Patch.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/patch/PatchLibrary.cpp")

# M8: AI generation (SPEC §6): settings, the chat-completions client, prompt
# builder, response parser and the generate/self-heal pipeline. JUCE-based
# like Patch.cpp, so the plugin and vibesynth_cli (--batch) each compile it.
set(VIBESYNTH_AI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ai/AiSettings.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ai/MetaClient.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ai/PromptBuilder.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ai/ResponseParser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ai/Generator.cpp")
# M11 (SPEC §9.7): Mutate. (Node ids, src/ai/NodeId.cpp, are JUCE-free and
# live in vibesynth_engine with the saved tweaks that use them.)
list(APPEND VIBESYNTH_AI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ai/Mutator.cpp")

# M8: prompt resources (SPEC §6.1), embedded for the plugin and the CLI.
# BinaryData keeps only file names, so names must be unique across the
# folders (examples are example_*.md).
file(GLOB VIBESYNTH_PROMPT_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/prompts/*.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/prompts/recipes/*.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/prompts/examples/*.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/prompts/edit/*.md")
juce_add_binary_data(PulsarPromptData
    HEADER_NAME PulsarPromptData.h
    NAMESPACE PulsarPromptData
    SOURCES ${VIBESYNTH_PROMPT_FILES})

# M5: built-in presets (SPEC §9.4), embedded for the plugin and the CLI.
# juce_add_binary_data makes a plain static library (generated .cpp, no
# JUCE module sources), so linking it into both is safe. Order here is not
# the program order; that lives in Patch.cpp (kBuiltins).
juce_add_binary_data(PulsarPresetData
    HEADER_NAME PulsarPresetData.h
    NAMESPACE PulsarPresetData
    SOURCES
        resources/presets/reese.pulsar
        resources/presets/scalpel.pulsar
        resources/presets/wonk_dive.pulsar
        resources/presets/tape_stop.pulsar
        resources/presets/coupled_scream.pulsar
        resources/presets/corroder.pulsar
        resources/presets/boing.pulsar
        resources/presets/micro_edit.pulsar
        resources/presets/sub.pulsar
        resources/presets/garage_organ.pulsar
        resources/presets/twostep_stab.pulsar
        resources/presets/pluck.pulsar
        resources/presets/glass_mallet.pulsar
        resources/presets/mono_lead.pulsar
        resources/presets/velvet_pad.pulsar)

# M9: the editor's fonts (SPEC §9.5; OFL-1.1, see THIRD_PARTY_LICENSES.md).
# Plugin only; the licence texts sit next to them in resources/fonts.
juce_add_binary_data(PulsarFontData
    HEADER_NAME PulsarFontData.h
    NAMESPACE PulsarFontData
    SOURCES
        resources/fonts/Inter-Regular.ttf
        resources/fonts/Inter-Medium.ttf
        resources/fonts/Inter-SemiBold.ttf
        resources/fonts/JetBrainsMonoNL-Regular.ttf)

# M9: the editor (SPEC §9.5, §9.6, §12 src/ui).
set(VIBESYNTH_UI_SOURCES
    src/ui/PulsarLookAndFeel.cpp
    src/ui/PromptPanel.cpp
    src/ui/MacroPanel.cpp
    src/ui/NodeLayout.cpp
    src/ui/NodeView.cpp
    src/ui/NodeInspector.cpp
    src/ui/PresetBrowser.cpp
    src/ui/CodeView.cpp
    src/ui/SettingsPanel.cpp
    src/ui/Snapshot.cpp)

target_sources(VibeSynthVST PRIVATE
    src/PluginProcessor.cpp
    src/PluginProcessorTweaks.cpp
    src/PluginEditor.cpp
    ${VIBESYNTH_UI_SOURCES}
    ${VIBESYNTH_PATCH_SOURCES}
    ${VIBESYNTH_AI_SOURCES})

# M11 (SPEC §9.7): Mutate, Change this node and Undo in the processor and
# the editor (audition strip, its review snapshots; "Change this node" is
# the M10 inspector's text box).
target_sources(VibeSynthVST PRIVATE
    src/ProcessorEdit.cpp
    src/ui/AuditionStrip.cpp
    src/ui/SnapshotM11.cpp
    src/ui/ModStrip.cpp
    src/ui/SnapshotM13.cpp)

target_include_directories(VibeSynthVST PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(VibeSynthVST PUBLIC
    JUCE_VST3_CAN_REPLACE_VST2=0)

# The plugin runs the shared engine (eel2 + DaisySP lexicon + PatchEngine);
# see the vibesynth_engine target below. No engine .cpp here directly.
target_link_libraries(VibeSynthVST PRIVATE
    vibesynth_engine
    PulsarPresetData
    PulsarPromptData
    PulsarFontData
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# M4: the shared DSP engine, linked into the plugin and the CLI alike
# (SPEC §10.3) so both run exactly the same code. Lexicon + PatchEngine,
# backed by eel2 and DaisySP (MIT pin above). M5 adds PatchProgram and the
# offline validator (JUCE-free; Patch.h is header-only from here). M6 adds
# the loop-nesting check and PatchPlayer (voices + master + output stage,
# moved out of the processor so vibesynth_cli --render plays the same chain).
add_library(vibesynth_engine STATIC
    src/patch/PatchEngine.cpp
    src/patch/PatchValidator.cpp
    src/patch/LoopNesting.cpp
    src/patch/LexiconLint.cpp
    src/patch/PatchPlayer.cpp
    src/engine/NodeMeters.cpp
    src/engine/Wavetables.cpp
    src/engine/LexiconRacks.cpp
    src/engine/Racks.cpp
    src/engine/Racks2.cpp
    src/engine/LexiconFn1.cpp
    src/engine/LexiconFn2.cpp
    src/engine/LexiconFn3.cpp
    src/engine/LexiconFn4.cpp
    src/engine/LexiconFn5.cpp
    src/engine/LexiconFn6.cpp
    src/engine/LexiconReg.cpp
    src/engine/LexiconHooks.cpp
    src/engine/NodeOverrides.cpp
    src/engine/Modulators.cpp
    src/ai/NodeId.cpp
    src/patch/PatchTweaks.cpp
    src/patch/PatchBake.cpp)
# M10: the override maths must round like EEL2's separate multiply and add
# (vibesynth_cli --bake output renders identically), so no FMA contraction.
set_source_files_properties(src/engine/NodeOverrides.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-ffp-contract=off>")
target_include_directories(vibesynth_engine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/WDL/WDL")
target_link_libraries(vibesynth_engine PUBLIC eel2 daisysp_mit)
# WDL's wdltypes.h includes <windows.h> without NOMINMAX, so every TU that
# includes PatchEngine.h (via the EEL2 headers) would get the min/max
# macros and break std::min/std::max. PUBLIC: reaches VibeSynthVST and
# vibesynth_cli too. No global WIN32_LEAN_AND_MEAN (JUCE manages its own
# Windows includes).
target_compile_definitions(vibesynth_engine PUBLIC
    $<$<PLATFORM_ID:Windows>:NOMINMAX>)
# Linked into the VST3 shared module, so it must be position independent
# (same as eel2 and daisysp_mit).
set_target_properties(vibesynth_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
# -O3 for engine code (SPEC §11); never fast-math (see daisysp_mit above).
target_compile_options(vibesynth_engine PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-O3;-fno-fast-math>
    $<$<CXX_COMPILER_ID:MSVC>:/O2>)

# Headless test tool (SPEC §10.3). Console target sharing the engine code;
# CI runs --smoketest and --selftest (gating); --bench is report-only.
# Engine headers come through vibesynth_engine's public include dirs (no
# repo-root include path, repo rule 10). M5: a JUCE console app so it
# compiles the same Patch.cpp as the plugin; no web browser. M6:
# juce_audio_formats for --render's WAV writer. M8: the AI sources for
# --batch, so curl on Linux (as the plugin) and juce_data_structures for
# the settings file.
set(VIBESYNTH_CLI_CURL FALSE)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(VIBESYNTH_CLI_CURL TRUE)
endif()
juce_add_console_app(vibesynth_cli
    NEEDS_CURL ${VIBESYNTH_CLI_CURL}
    NEEDS_WEB_BROWSER FALSE)
target_sources(vibesynth_cli PRIVATE
    tools/vibesynth_cli/main.cpp
    tools/vibesynth_cli/Bench.cpp
    tools/vibesynth_cli/Render.cpp
    tools/vibesynth_cli/SmokeM7.cpp
    tools/vibesynth_cli/SmokeM8.cpp
    tools/vibesynth_cli/Batch.cpp
    tools/vibesynth_cli/SmokeM9.cpp
    tools/vibesynth_cli/SmokeM10.cpp
    tools/vibesynth_cli/Bake.cpp
    src/ui/NodeLayout.cpp
    ${VIBESYNTH_PATCH_SOURCES}
    ${VIBESYNTH_AI_SOURCES})
# M11: --change, --mutate, --change-batch and the M11 smoketests; the
# M10 + M11 merge checks.
target_sources(vibesynth_cli PRIVATE
    tools/vibesynth_cli/Edit.cpp
    tools/vibesynth_cli/SmokeM11.cpp
    tools/vibesynth_cli/SmokeM12.cpp
    tools/vibesynth_cli/SmokeMerge.cpp
    tools/vibesynth_cli/SmokeM13.cpp)
# --render writes WAV only: keep JUCE's bundled FLAC/Ogg Vorbis codecs (and
# the Windows Media reader) out of the CLI.
target_compile_definitions(vibesynth_cli PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_FLAC=0
    JUCE_USE_OGGVORBIS=0
    JUCE_USE_MP3AUDIOFORMAT=0
    JUCE_USE_WINDOWS_MEDIA_FORMAT=0)

# Deliberately broken patches (tests/patches, not shipped): embedded in the
# CLI only, so --smoketest does not depend on the working directory.
# ok_nested is the passing two-level case for the M6 loop-nesting check.
juce_add_binary_data(PulsarTestPatches
    HEADER_NAME PulsarTestPatches.h
    NAMESPACE PulsarTestPatches
    SOURCES
        tests/patches/broken_nan.pulsar
        tests/patches/broken_loud.pulsar
        tests/patches/broken_dc.pulsar
        tests/patches/broken_silent.pulsar
        tests/patches/broken_compile_error.pulsar
        tests/patches/broken_slow.pulsar
        tests/patches/broken_bad_json.pulsar
        tests/patches/broken_missing_voice.pulsar
        tests/patches/broken_oversize.pulsar
        tests/patches/broken_nested.pulsar
        tests/patches/broken_lint.pulsar
        tests/patches/ok_nested.pulsar)

target_link_libraries(vibesynth_cli PRIVATE
    vibesynth_engine
    PulsarPresetData
    PulsarPromptData
    PulsarTestPatches
    juce::juce_core
    juce::juce_data_structures
    juce::juce_audio_formats
    juce::juce_recommended_config_flags)
# Keep the binary at build/vibesynth_cli (CI and docs call it there), not
# in JUCE's vibesynth_cli_artefacts/<config> folder.
set_target_properties(vibesynth_cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
