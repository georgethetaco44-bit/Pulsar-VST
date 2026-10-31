// M11 review snapshots. See SnapshotM11.h.

#include "ui/SnapshotM11.h"

#include <cstdio>
#include <cstring>

#include "PluginEditor.h"

namespace vibesynth::ui {

namespace {

MutateView fakeView (bool finished)
{
    using State = MutateSlotView::State;
    MutateView v;
    v.open = true;
    v.running = ! finished;
    v.baseName = "Reese";
    v.startedMs = juce::Time::getMillisecondCounterHiRes() - 9400.0;
    v.auditioning = finished ? 2 : 0;
    const char* dirs[] = { "Darker", "Movement", "Brighter", "Weirder" };
    for (int i = 0; i < MutateView::kSlots; ++i)
        v.slots[(size_t) i].direction = dirs[i];
    v.slots[0] = { State::Ready, "Darker", "Abyss Reese", "Ready", "" };
    v.slots[1] = { State::Failed, "Movement",  "", "too loud: peak +14.2 dBFS (max +12) at note 28, macros all 1",
                   "too loud: peak +14.2 dBFS (max +12) at note 28, macros all 1 (after 2 retries)" };
    if (finished)
    {
        v.slots[2] = { State::Ready, "Brighter", "Glass Reese", "Ready", "" };
        v.slots[3] = { State::Ready, "Weirder", "Lorenz Throat", "Ready", "" };
    }
    else
    {
        v.slots[2] = { State::Testing, "Brighter", "", "Testing (retry 1/2)", "" };
        v.slots[3] = { State::Waiting, "Weirder", "", "Waiting for model", "" };
    }
    return v;
}

} // namespace

void runM11Snapshots (VibeSynthAudioProcessorEditor& editor, const std::function<void (const juce::String&)>& shoot)
{
    editor.showPage (0);
    editor.setMutateOverride (fakeView (false));
    shoot ("audition");
    editor.setMutateOverride (fakeView (true));
    shoot ("audition_done");
    editor.setMutateOverride (std::nullopt);

    editor.openChangePopup ("flt_lp:0", "make this filter wobble at 1/4 notes");
    shoot ("change_popup");
    editor.closeChangePopup();

    // End-to-end Undo: patch and exact macro values after a preset load.
    auto& p = editor.getProcessor();
    auto waitFor = [&p] (const std::function<bool()>& done)
    {
        for (int i = 0; i < 500 && ! done(); ++i)
        {
            juce::Thread::sleep (10);
            p.deliverPendingResults();
        }
        return done();
    };
    auto macros = [&p]
    {
        std::array<float, VibeSynthAudioProcessor::numMacros> m {};
        for (int i = 0; i < VibeSynthAudioProcessor::numMacros; ++i)
            m[(size_t) i] = p.apvts.getRawParameterValue ("macro" + juce::String (i + 1))->load();
        return m;
    };
    const float odd[] = { 0.1234567f, 0.9876543f, 0.000001f, 1.0f, 0.3333333f, 0.7071068f, 0.5f, 0.0f };
    for (int i = 0; i < VibeSynthAudioProcessor::numMacros; ++i)
        p.apvts.getParameter ("macro" + juce::String (i + 1))->setValueNotifyingHost (odd[i]);
    const vibesynth::Patch before = p.getUiPatch();
    const auto beforeMacros = macros();
    const int depth0 = p.canUndo() ? 1 : 0;
    int v = p.getPatchVersion();
    p.loadBuiltin (2);
    const bool loaded = waitFor ([&] { return p.getPatchVersion() != v; });
    const bool changed = p.getUiPatch().voice != before.voice && macros() != beforeMacros;
    v = p.getPatchVersion();
    p.undo();
    const bool undone = waitFor ([&] { return p.getPatchVersion() != v; });
    const auto afterMacros = macros();
    const bool samePatch = p.getUiPatch() == before;
    const bool sameMacros = std::memcmp (afterMacros.data(), beforeMacros.data(), sizeof (float) * afterMacros.size()) == 0;
    std::printf ("m11check: undo after a preset load restored '%s' and all 8 macro values bit-exactly: %s "
                 "(loaded %d, changed %d, undone %d, patch %d, macros %d, stack had entries %d)\n",
                 before.name.c_str(), loaded && changed && undone && samePatch && sameMacros ? "yes" : "NO",
                 loaded ? 1 : 0, changed ? 1 : 0, undone ? 1 : 0, samePatch ? 1 : 0, sameMacros ? 1 : 0,
                 depth0);
    std::fflush (stdout);
}

} // namespace vibesynth::ui
