#include "patch/WavetableLoader.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <exception>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#include "ai/AiSettings.h"
#include "engine/LoadedWavetables.h"

namespace vibesynth {

namespace {

constexpr double kPi = 3.14159265358979323846;

void radix2Fft (std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * kPi / static_cast<double> (len);
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

WavetableFolderStatus gStatus;
std::mutex gStatusMutex;   // held only to read or change gStatus, never while loading

// The scan thread. gJobMutex guards the handle and the user count, and stays held
// while joining, so a new user can't start a second scan beside one that is
// still stopping. The thread itself never takes it.
std::mutex gJobMutex;
std::thread gThread;
int gUsers = 0;
std::atomic<bool> gFinished { false };  // the whole folder has been scanned
std::atomic<bool> gCancel { false };
std::vector<std::string> gDoneFiles; // scan thread only: published, so a resumed scan skips them

juce::File wavetableFolder()
{
    return juce::File (pulsarDataFolder()).getChildFile ("Wavetables");
}

void noteSkipped (const std::string& fileName, const std::string& reason)
{
    std::fprintf (stderr, "[Wavetables] skipped %s: %s\n", fileName.c_str(), reason.c_str());
    const std::lock_guard<std::mutex> lock (gStatusMutex);
    gStatus.skipped.push_back (fileName + ": " + reason);
}

std::unique_ptr<LoadedWavetable> buildFromReader (juce::AudioFormatReader& reader, const juce::File& file,
                                                  std::string& reason, const std::atomic<bool>* cancel)
{
    if (reader.numChannels < 1)
    {
        reason = "no audio channels";
        return nullptr;
    }

    const int64_t lengthSamples = reader.lengthInSamples;
    if (lengthSamples <= 0 || (lengthSamples % kBaseWtFrameSize) != 0)
    {
        reason = "length must be a whole multiple of 2048 samples (got " + std::to_string (lengthSamples) + ")";
        return nullptr;
    }

    const int64_t frameCount = lengthSamples / kBaseWtFrameSize;
    if (frameCount < 1 || frameCount > kMaxWtFrames)
    {
        reason = "frame count must be 1..256 (got " + std::to_string (frameCount) + ")";
        return nullptr;
    }
    const int numFrames = static_cast<int> (frameCount);

    // Read channel 0 (left)
    juce::AudioBuffer<float> buf (1, static_cast<int> (lengthSamples));
    if (! reader.read (&buf, 0, static_cast<int> (lengthSamples), 0, true, false))
    {
        reason = "error reading sample data";
        return nullptr;
    }

    if (! normaliseWavetableFrames (buf.getWritePointer (0), numFrames))
    {
        reason = "the file is silent";
        return nullptr;
    }

    const float* src = buf.getReadPointer (0);
    auto wt = std::make_unique<LoadedWavetable>();
    wt->name = file.getFileNameWithoutExtension().toStdString();
    wt->numFrames = numFrames;
    wt->data.resize (static_cast<size_t> (numFrames) * static_cast<size_t> (kLoadedWtFrameStride), 0.0f);

    // Build 11 FFT brickwall mips for each frame
    std::vector<std::complex<double>> spectrum (kBaseWtFrameSize);
    std::complex<double>* X = spectrum.data();
    for (int frame = 0; frame < numFrames; ++frame)
    {
        if (cancel != nullptr && cancel->load (std::memory_order_relaxed))
            return nullptr;

        const float* frameSrc = src + frame * kBaseWtFrameSize;
        for (int n = 0; n < kBaseWtFrameSize; ++n)
            X[n] = static_cast<double> (frameSrc[n]);

        radix2Fft (spectrum);

        // Compute mips 0..10
        for (int mip = 0; mip < kNumLoadedWtMips; ++mip)
        {
            const int N = kBaseWtFrameSize >> mip;
            const int mipOff = loadedWtMipOffset (mip);
            float* mipRow = wt->data.data() + frame * kLoadedWtFrameStride + mipOff;

            std::vector<std::complex<double>> mipSpectrum (static_cast<size_t> (N), 0.0);
            std::complex<double>* Y = mipSpectrum.data();
            Y[0] = X[0];
            for (int k = 1; k < N / 2; ++k)
            {
                Y[k] = X[k];
                Y[N - k] = X[kBaseWtFrameSize - k];
            }
            Y[N / 2] = std::complex<double> (X[N / 2].real(), 0.0);

            // Inverse FFT: conj -> FFT -> conj / N
            for (int k = 0; k < N; ++k)
                Y[k] = std::conj (Y[k]);
            radix2Fft (mipSpectrum);
            for (int n = 0; n < N; ++n)
            {
                const double val = std::conj (Y[n]).real() / static_cast<double> (kBaseWtFrameSize);
                mipRow[n] = static_cast<float> (clampSafe (val, 2.0));
            }
            mipRow[N] = mipRow[0]; // guard sample
        }
    }
    return wt;
}

void scanFolder()
{
    const juce::File wtFolder = wavetableFolder();
    if (! wtFolder.exists())
    {
        wtFolder.createDirectory();
        return;
    }

    auto files = wtFolder.findChildFiles (juce::File::findFiles, false, "*.wav");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) {
        return a.getFileName().compareIgnoreCase (b.getFileName()) < 0;
    });

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    int slot = static_cast<int> (gDoneFiles.size());
    for (const auto& f : files)
    {
        if (gCancel.load (std::memory_order_relaxed) || slot >= kMaxLoadedWavetables)
            break;

        const std::string fileName = f.getFileName().toStdString();
        if (std::find (gDoneFiles.begin(), gDoneFiles.end(), fileName) != gDoneFiles.end())
            continue;

        std::string reason;
        std::unique_ptr<LoadedWavetable> wt;
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (f));
        if (reader == nullptr)
            reason = "could not read WAV file";
        else
            wt = buildFromReader (*reader, f, reason, &gCancel);

        if (wt == nullptr)
        {
            if (! reason.empty())
                noteSkipped (fileName, reason);
            continue;
        }

        const int tableNumber = 8 + slot;
        const int numFrames = wt->numFrames;
        const std::string name = wt->name;
        std::fprintf (stderr, "[Wavetables] loaded table %d: %s (%d frames)\n", tableNumber, name.c_str(), numFrames);
        setLoadedWavetable (slot, std::move (wt));
        gDoneFiles.push_back (fileName);
        ++slot;
        const std::lock_guard<std::mutex> lock (gStatusMutex);
        gStatus.loaded.push_back ({ tableNumber, name, numFrames });
    }
}

void scanThreadMain()
{
    {
        // A resumed scan meets the skipped files again.
        const std::lock_guard<std::mutex> lock (gStatusMutex);
        gStatus.skipped.clear();
    }
    try
    {
        scanFolder();
    }
    catch (const std::exception& e)
    {
        // Out of memory on a big folder, say: the tables so far still work.
        noteSkipped ("(folder scan stopped)", e.what());
    }
    catch (...)
    {
        noteSkipped ("(folder scan stopped)", "unknown error");
    }

    const bool completed = ! gCancel.load (std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> lock (gStatusMutex);
        gStatus.loading = false;
    }
    if (completed)
        gFinished.store (true, std::memory_order_release);
}

// Caller holds gJobMutex.
void startScanIfNeeded()
{
    if (gFinished.load (std::memory_order_acquire) || gThread.joinable())
        return;
    {
        const std::lock_guard<std::mutex> lock (gStatusMutex);
        gStatus.folderPath = wavetableFolder().getFullPathName().toStdString();
        gStatus.loading = true;
    }
    gCancel.store (false, std::memory_order_relaxed);
    try
    {
        gThread = std::thread (scanThreadMain);
    }
    catch (const std::system_error& e)
    {
        // No thread to be had: the plugin still works on the built-in tables.
        std::fprintf (stderr, "[Wavetables] no scan thread: %s\n", e.what());
        const std::lock_guard<std::mutex> lock (gStatusMutex);
        gStatus.loading = false;
    }
}

} // namespace

bool normaliseWavetableFrames (float* samples, int numFrames) noexcept
{
    if (samples == nullptr || numFrames < 1)
        return false;

    double peak = 0.0;
    for (int frame = 0; frame < numFrames; ++frame)
    {
        float* s = samples + frame * kBaseWtFrameSize;
        double sum = 0.0;
        for (int n = 0; n < kBaseWtFrameSize; ++n)
        {
            s[n] = sanitizeFloat (s[n]);
            sum += static_cast<double> (s[n]);
        }
        const double mean = sum / static_cast<double> (kBaseWtFrameSize);
        for (int n = 0; n < kBaseWtFrameSize; ++n)
        {
            const double v = static_cast<double> (s[n]) - mean;
            s[n] = static_cast<float> (v);
            peak = std::max (peak, std::fabs (v));
        }
    }

    if (peak < 1.0e-6) // -120 dBFS
        return false;

    const double gain = static_cast<double> (kLoadedWtPeakLevel) / peak;
    const int total = numFrames * kBaseWtFrameSize;
    for (int n = 0; n < total; ++n)
        samples[n] = static_cast<float> (static_cast<double> (samples[n]) * gain);
    return true;
}

std::unique_ptr<LoadedWavetable> buildLoadedWavetable (const std::string& path, std::string& reason,
                                                       const std::atomic<bool>* cancel)
{
    reason.clear();
    const juce::File file { juce::String (path) };
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
    {
        reason = "could not read WAV file";
        return nullptr;
    }
    return buildFromReader (*reader, file, reason, cancel);
}

void beginWavetableLoad()
{
    const std::lock_guard<std::mutex> lock (gJobMutex);
    ++gUsers;
    startScanIfNeeded();
}

void endWavetableLoad()
{
    const std::lock_guard<std::mutex> lock (gJobMutex);
    if (gUsers > 0)
        --gUsers;
    if (gUsers > 0 || ! gThread.joinable())
        return;
    gCancel.store (true, std::memory_order_relaxed);
    gThread.join();
}

void loadWavetablesFromFolder()
{
    const std::lock_guard<std::mutex> lock (gJobMutex);
    startScanIfNeeded();
    if (gThread.joinable())
        gThread.join();
}

WavetableFolderStatus getWavetableFolderStatus()
{
    const std::lock_guard<std::mutex> lock (gStatusMutex);
    if (gStatus.folderPath.empty())
        gStatus.folderPath = wavetableFolder().getFullPathName().toStdString();
    return gStatus;
}

} // namespace vibesynth
