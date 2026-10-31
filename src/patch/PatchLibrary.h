#pragma once

// M9 (SPEC §9.5): the user's patch folders, next to the settings file in
// the "Pulsar" app-data folder:
//   Patches/    user-saved patches (the preset browser's User section)
//   Generated/  every successful generation, auto-saved, newest kept
//               (the History tab)
//
// Uses juce::File, so like Patch.cpp it is compiled into the plugin and
// vibesynth_cli (VIBESYNTH_PATCH_SOURCES). Message thread or a worker,
// never the audio thread. Every function takes the folder, so tests can
// point it at a temporary directory.

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

#include "patch/Patch.h"

namespace vibesynth {

struct PatchLibrary
{
    static constexpr int kHistoryLimit = 50;

    static juce::File userFolder();     // <app data>/Patches
    static juce::File historyFolder();  // <app data>/Generated

    // *.pulsar in `folder`, sorted by name (case-insensitive). Names only:
    // nothing is read.
    static juce::Array<juce::File> listPatches (const juce::File& folder);

    struct HistoryEntry
    {
        juce::File file;
        juce::String name;    // the patch's name
        juce::String prompt;  // the prompt that generated it
        juce::Time time;      // from the file name (written at save)
    };
    // Newest first, at most `limit`. Reads each file (they are small; the
    // folder holds at most kHistoryLimit).
    static std::vector<HistoryEntry> listHistory (const juce::File& folder, int limit = kHistoryLimit);

    // Writes `patch` as <folder>/<name>.pulsar with its name set to `name`
    // (creating the folder). Fails if the file exists and `overwrite` is
    // false. Returns the file, or an empty File with `error` set.
    static juce::File saveUserPatch (const juce::File& folder, const juce::String& name, Patch patch,
                                     bool overwrite, juce::String& error);
    static juce::File userPatchFile (const juce::File& folder, const juce::String& name);

    // Writes a generated patch as <folder>/<yyyymmdd-hhmmss>-<name>.pulsar
    // and deletes all but the newest `limit` files. Returns the file, or
    // an empty File with `error` set.
    static juce::File addToHistory (const juce::File& folder, const Patch& patch, juce::Time now,
                                    juce::String& error, int limit = kHistoryLimit);
    static void pruneHistory (const juce::File& folder, int limit = kHistoryLimit);
};

} // namespace vibesynth
