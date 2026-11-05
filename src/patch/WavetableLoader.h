#pragma once

// Wavetable loader (Task 3).
//
// At startup, scans <app-data>/Wavetables/ for .wav files, sorted by name,
// up to 32 files. Checks that length is a whole number of 2048-sample frames
// (up to 256 frames), builds 11 FFT brickwall band-limited mips, and publishes
// them to LoadedWavetables. Runs off the audio thread only.

#include <string>
#include <vector>

namespace vibesynth {

struct LoadedFileInfo
{
    int tableNumber = 0; // 8..39
    std::string name;
    int numFrames = 0;
};

struct WavetableFolderStatus
{
    std::string folderPath;
    std::vector<LoadedFileInfo> loaded;
    std::vector<std::string> skipped;
};

// Scans <app-data>/Wavetables/, loads valid files, and publishes them.
// Thread-safe; runs once off the audio thread.
void loadWavetablesFromFolder();

// Returns status for the UI Settings panel.
WavetableFolderStatus getWavetableFolderStatus();

} // namespace vibesynth
