// M10: vibesynth_cli --bake in.pulsar out.pulsar (SPEC §9.6). Folds the
// patch's tweaks and macro maps into its code (patch/PatchBake.h), runs the
// offline test on the result as a file load would (the computed trim
// replaces the stored one) and writes it.

#include "SmokeM10.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdio>

#include "patch/PatchBake.h"
#include "patch/PatchValidator.h"

namespace vibesynth_cli {

int runBake (const char* inPath, const char* outPath, int oversampling)
{
    const juce::File inFile = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (inPath));
    const juce::File outFile = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (outPath));
    if (! inFile.existsAsFile())
    {
        std::printf ("cannot open %s\n", inPath);
        return 2;
    }
    const auto parsed = vibesynth::parsePatch (inFile.loadFileAsString().toStdString());
    if (! parsed.ok())
    {
        std::printf ("REJECTED (%s): %s\n", vibesynth::patchStatusName (parsed.status), parsed.error.c_str());
        return 1;
    }
    const auto& in = parsed.patch;
    if (in.tweaks.empty() && in.macroMaps.empty())
        std::printf ("note: %s has no tweaks or macro maps; writing it unchanged\n", inPath);

    auto baked = vibesynth::bakePatch (in);
    if (! baked.ok)
    {
        std::printf ("BAKE REFUSED: %s\n", baked.error.c_str());
        return 1;
    }
    for (const auto& n : baked.notes)
        std::printf ("note: %s\n", n.c_str());

    const auto vr = vibesynth::validatePatch (baked.patch, 48000.0, nullptr, oversampling);
    if (! vr.ok())
    {
        std::printf ("REJECTED after baking (%s): %s\n", vibesynth::patchStatusName (vr.status), vr.error.c_str());
        return 1;
    }
    baked.patch.trim = std::fmax (1.0e-6, vibesynth::quantisePatchNumber (vr.trim));

    if (! outFile.getParentDirectory().createDirectory() || ! outFile.replaceWithText (vibesynth::serializePatch (baked.patch)))
    {
        std::printf ("cannot write %s\n", outPath);
        return 1;
    }
    std::printf ("baked %s -> %s: %d calls rewritten, offline test passed, trim %+.1f dB (was %+.1f)\n",
                 in.name.c_str(), outFile.getFullPathName().toRawUTF8(), baked.callsChanged,
                 20.0 * std::log10 (baked.patch.trim), 20.0 * std::log10 (in.trim));
    return 0;
}

} // namespace vibesynth_cli
