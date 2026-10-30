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

target_compile_definitions(VibeSynthVST PUBLIC
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(VibeSynthVST PRIVATE
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
