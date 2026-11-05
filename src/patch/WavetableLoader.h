#pragma once

// Wavetable loader.
//
// Scans <app-data>/Wavetables/ for .wav files, sorted by name, up to 32 files.
// A file's length must be a whole number of 2048-sample frames (up to 256).
// Each table is normalised (DC removed from every frame, then one gain for the
// whole table), gets 11 FFT brickwall band-limited mips and is published to
// LoadedWavetables. Runs off the audio thread only.
//
// The plugin must not hold up a host that is opening or scanning it, so the
// scan runs on a background thread. A table plays as table 0 until it is
// published. The CLI waits for the scan, so its renders are repeatable.

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace vibesynth {

struct LoadedWavetable;

// The level the loudest sample of a loaded table is scaled to, leaving a little
// room for the overshoot of its band-limited mips.
constexpr float kLoadedWtPeakLevel = 0.9f;

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
    bool loading = false;
};

// One user of the tables (a plugin instance) starts, and returns at once. The
// scan starts with the first user, and runs once per process unless cancelled.
void beginWavetableLoad();

// That user is done. When it was the last, a scan still running is cancelled
// and joined before this returns, within about one frame's work, so the plugin's
// code can be unloaded right after. A later beginWavetableLoad() finishes the
// scan. Tables already published stay.
void endWavetableLoad();

// Starts the scan if needed and waits until it has finished (the CLI).
void loadWavetablesFromFolder();

// Returns status for the UI Settings panel. Any non-audio thread; never waits
// for a table to finish.
WavetableFolderStatus getWavetableFolderStatus();

// Removes each frame's DC, then scales the whole table so its largest sample
// reaches kLoadedWtPeakLevel; the frames keep their levels relative to each
// other. `samples` holds numFrames * 2048 values. Non-finite samples become 0.
// Returns false if the table is silent (nothing to scale it from).
bool normaliseWavetableFrames (float* samples, int numFrames) noexcept;

// Reads and prepares one file without publishing it. Null with `reason` set if
// the file can't be used; null with `reason` empty if `cancel` was set.
std::unique_ptr<LoadedWavetable> buildLoadedWavetable (const std::string& path, std::string& reason,
                                                       const std::atomic<bool>* cancel = nullptr);

} // namespace vibesynth
