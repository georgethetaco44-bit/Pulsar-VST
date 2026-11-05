#include "patch/WavetableLoader.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <complex>
#include <cstdio>
#include <mutex>
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
std::mutex gStatusMutex;
std::once_flag gLoadedOnce;

void doLoadWavetables()
{
    std::lock_guard<std::mutex> lock (gStatusMutex);
    const juce::File dataDir (pulsarDataFolder());
    const juce::File wtFolder = dataDir.getChildFile ("Wavetables");
    gStatus.folderPath = wtFolder.getFullPathName().toStdString();

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

    int slot = 0;
    for (const auto& f : files)
    {
        if (slot >= kMaxLoadedWavetables)
            break;

        const std::string fileName = f.getFileName().toStdString();
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (f));
        if (reader == nullptr)
        {
            const std::string reason = "could not read WAV file";
            std::fprintf (stderr, "[Wavetables] skipped %s: %s\n", fileName.c_str(), reason.c_str());
            gStatus.skipped.push_back (fileName + ": " + reason);
            continue;
        }

        if (reader->numChannels < 1)
        {
            const std::string reason = "no audio channels";
            std::fprintf (stderr, "[Wavetables] skipped %s: %s\n", fileName.c_str(), reason.c_str());
            gStatus.skipped.push_back (fileName + ": " + reason);
            continue;
        }

        const int64_t lengthSamples = reader->lengthInSamples;
        if (lengthSamples <= 0 || (lengthSamples % kBaseWtFrameSize) != 0)
        {
            const std::string reason = "length must be a whole multiple of 2048 samples (got "
                                       + std::to_string (lengthSamples) + ")";
            std::fprintf (stderr, "[Wavetables] skipped %s: %s\n", fileName.c_str(), reason.c_str());
            gStatus.skipped.push_back (fileName + ": " + reason);
            continue;
        }

        const int numFrames = static_cast<int> (lengthSamples / kBaseWtFrameSize);
        if (numFrames < 1 || numFrames > kMaxWtFrames)
        {
            const std::string reason = "frame count must be 1..256 (got " + std::to_string (numFrames) + ")";
            std::fprintf (stderr, "[Wavetables] skipped %s: %s\n", fileName.c_str(), reason.c_str());
            gStatus.skipped.push_back (fileName + ": " + reason);
            continue;
        }

        // Read channel 0 (left)
        juce::AudioBuffer<float> buf (1, static_cast<int> (lengthSamples));
        if (! reader->read (&buf, 0, static_cast<int> (lengthSamples), 0, true, false))
        {
            const std::string reason = "error reading sample data";
            std::fprintf (stderr, "[Wavetables] skipped %s: %s\n", fileName.c_str(), reason.c_str());
            gStatus.skipped.push_back (fileName + ": " + reason);
            continue;
        }

        const float* src = buf.getReadPointer (0);
        auto wt = std::make_unique<LoadedWavetable>();
        wt->name = f.getFileNameWithoutExtension().toStdString();
        wt->numFrames = numFrames;
        wt->data.resize (static_cast<size_t> (numFrames) * static_cast<size_t> (kLoadedWtFrameStride), 0.0f);

        // Build 11 FFT brickwall mips for each frame
        std::vector<std::complex<double>> X (kBaseWtFrameSize);
        for (int frame = 0; frame < numFrames; ++frame)
        {
            const float* frameSrc = src + frame * kBaseWtFrameSize;
            for (int n = 0; n < kBaseWtFrameSize; ++n)
                X[n] = static_cast<double> (frameSrc[n]);

            radix2Fft (X);

            // Compute mips 0..10
            for (int mip = 0; mip < kNumLoadedWtMips; ++mip)
            {
                const int N = kBaseWtFrameSize >> mip;
                const int mipOff = loadedWtMipOffset (mip);
                float* mipRow = wt->data.data() + frame * kLoadedWtFrameStride + mipOff;

                std::vector<std::complex<double>> Y (N, 0.0);
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
                radix2Fft (Y);
                for (int n = 0; n < N; ++n)
                {
                    const double val = std::conj (Y[n]).real() / static_cast<double> (kBaseWtFrameSize);
                    mipRow[n] = static_cast<float> (clampSafe (val, 2.0));
                }
                mipRow[N] = mipRow[0]; // guard sample
            }
        }

        const int tableNumber = 8 + slot;
        std::fprintf (stderr, "[Wavetables] loaded table %d: %s (%d frames)\n",
                      tableNumber, wt->name.c_str(), numFrames);
        gStatus.loaded.push_back ({ tableNumber, wt->name, numFrames });
        setLoadedWavetable (slot, std::move (wt));
        ++slot;
    }
}

} // namespace

void loadWavetablesFromFolder()
{
    std::call_once (gLoadedOnce, doLoadWavetables);
}

WavetableFolderStatus getWavetableFolderStatus()
{
    std::lock_guard<std::mutex> lock (gStatusMutex);
    return gStatus;
}

} // namespace vibesynth
