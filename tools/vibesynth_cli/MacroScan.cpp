#include "MacroScan.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "patch/Patch.h"
#include "patch/PatchValidator.h"

namespace vibesynth_cli {

namespace {

int findBuiltinIndex (const juce::String& arg)
{
    const juce::String want = arg.endsWithIgnoreCase (".pulsar") ? arg.dropLastCharacters (7) : arg;
    for (int i = 0; i < vibesynth::numBuiltinPresets(); ++i)
    {
        const juce::String stem = juce::String (vibesynth::builtinPresetFile (i)).upToLastOccurrenceOf (".", false, false);
        if (want.equalsIgnoreCase (stem) || want.equalsIgnoreCase (juce::String (vibesynth::builtinPresetName (i))))
            return i;
    }
    return -1;
}

void printScanTable (const vibesynth::Patch& patch, const std::string& sourceName, int oversampling)
{
    const auto entries = vibesynth::scanPatchMacros (patch, 48000.0, oversampling);
    std::printf ("=== %s (%s) ===\n", patch.name.c_str(), sourceName.c_str());
    std::printf ("%-4s  %-16s  %8s  %8s  %8s  %10s  %s\n",
                 "Mac", "Label", "DefFlat", "At1Flat", "Delta", "Level(dB)", "Flag");
    std::printf ("----------------------------------------------------------------------\n");
    for (const auto& e : entries)
    {
        std::printf ("m%-3d  %-16s  %8.4f  %8.4f  %+8.4f  %+9.2f dB  %s\n",
                     e.macro,
                     e.label.c_str(),
                     e.defaultFlatness,
                     e.at1Flatness,
                     e.deltaFlatness,
                     e.levelChangeDb,
                     e.isNoise ? "[NOISE]" : "      ");
    }
    std::printf ("\n");
}

} // namespace

int runMacroScan (int argc, char** argv, int oversampling)
{
    bool scanAll = (argc == 0);
    if (argc >= 1)
    {
        const std::string arg = argv[0];
        if (arg == "all" || arg == "builtins")
            scanAll = true;
    }

    if (scanAll)
    {
        for (int i = 0; i < vibesynth::numBuiltinPresets(); ++i)
        {
            const std::string text = vibesynth::builtinPresetText (i);
            const auto pr = vibesynth::parsePatch (text);
            if (! pr.ok())
            {
                std::printf ("built-in %d (%s) failed to parse\n", i, vibesynth::builtinPresetFile (i));
                continue;
            }
            printScanTable (pr.patch, vibesynth::builtinPresetFile (i), oversampling);
        }
        return 0;
    }

    const juce::String arg (argv[0]);
    const juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile (arg);
    std::string text;
    std::string sourceName;
    if (f.existsAsFile())
    {
        text = f.loadFileAsString().toStdString();
        sourceName = f.getFileName().toStdString();
    }
    else
    {
        const int idx = findBuiltinIndex (arg);
        if (idx < 0)
        {
            std::printf ("no file or built-in named \"%s\"\n", arg.toRawUTF8());
            return 2;
        }
        text = vibesynth::builtinPresetText (idx);
        sourceName = vibesynth::builtinPresetFile (idx);
    }

    const auto pr = vibesynth::parsePatch (text);
    if (! pr.ok())
    {
        std::printf ("failed to parse %s: %s\n", sourceName.c_str(), pr.error.c_str());
        return 1;
    }

    printScanTable (pr.patch, sourceName, oversampling);
    return 0;
}

} // namespace vibesynth_cli
