// M10 (SPEC §9.6): the processor's side of the node inspector. Edits go
// into the active patch's tweak lists (so state, Save and the UI see them)
// and into the OverrideTable the audio thread reads. Message thread only.

#include "PluginProcessor.h"

#include "patch/PatchTweaks.h"

void VibeSynthAudioProcessor::applyTweaksFrom (const vibesynth::Patch& patch)
{
    vibesynth::applyPatchTweaks (patch, *overrideTable);
    ++tweaksVersion;
}

void VibeSynthAudioProcessor::editTweaks (const std::function<void (vibesynth::Patch&)>& edit)
{
    edit (uiPatch);
    {
        // The playing patch, unless a load has replaced it since the UI's.
        std::lock_guard<std::mutex> lock (patchMutex);
        if (activePatch.voice == uiPatch.voice && activePatch.init == uiPatch.init
            && activePatch.master == uiPatch.master)
        {
            activePatch.tweaks = uiPatch.tweaks;
            activePatch.macroMaps = uiPatch.macroMaps;
        }
    }
    applyTweaksFrom (uiPatch);
    sendChangeMessage();
}

void VibeSynthAudioProcessor::setNodeBypass (const std::string& node, bool on)
{
    editTweaks ([&] (vibesynth::Patch& p) { vibesynth::setPatchBypass (p, node, on); });
}

void VibeSynthAudioProcessor::setNodeArg (const std::string& node, const std::string& arg, int mode, double value)
{
    editTweaks ([&] (vibesynth::Patch& p) { vibesynth::setPatchArgTweak (p, node, arg, mode, value); });
}

void VibeSynthAudioProcessor::setNodeMacroMap (const std::string& node, const std::string& arg, int macro, double min,
                                               double max)
{
    editTweaks ([&] (vibesynth::Patch& p) { vibesynth::setPatchMacroMap (p, node, arg, macro, min, max); });
}

void VibeSynthAudioProcessor::clearNodeMacroMap (const std::string& node, const std::string& arg)
{
    editTweaks ([&] (vibesynth::Patch& p) { vibesynth::removePatchMacroMap (p, node, arg); });
}
