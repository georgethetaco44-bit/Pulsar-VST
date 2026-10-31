// M9 design review snapshots. See Snapshot.h.

#include "ui/Snapshot.h"

#include <cstdio>

#include "PluginEditor.h"
#include "ai/Generator.h"
#include "ui/SnapshotM11.h"
#include "ai/MetaClient.h"
#include "patch/PatchPlayer.h"
#include "patch/PatchValidator.h"

namespace vibesynth::ui {

namespace {

struct NoEvent
{
    int samplePosition = 0;
    const uint8_t* data = nullptr;
    int numBytes = 0;
};

const juce::String kPrompt = "weeping neuro reese with a throat that closes on the mod wheel";

} // namespace

std::unique_ptr<SnapshotDriver> SnapshotDriver::createIfRequested (VibeSynthAudioProcessorEditor& editor)
{
    if (editor.getProcessor().wrapperType != juce::AudioProcessor::wrapperType_Standalone)
        return {};
    const auto path = juce::SystemStats::getEnvironmentVariable ("PULSAR_SNAPSHOT", {});
    if (path.isEmpty())
        return {};
    return std::make_unique<SnapshotDriver> (editor, juce::File::getCurrentWorkingDirectory().getChildFile (path));
}

SnapshotDriver::SnapshotDriver (VibeSynthAudioProcessorEditor& e, juce::File d) : editor (e), dir (std::move (d))
{
    dir.createDirectory();
    startPatchVersion = editor.getProcessor().getPatchVersion();
    editor.getProcessor().loadBuiltin (0);
    startTimer (100);
}

SnapshotDriver::~SnapshotDriver()
{
    stopTimer();
}

void SnapshotDriver::timerCallback()
{
    auto& p = editor.getProcessor();
    // Wait (up to 15 s) for Reese and its node list.
    const bool ready = p.getPatchVersion() != startPatchVersion && p.areNodesKnown();
    if (! ready && ++waited < 150)
        return;
    stopTimer();
    runShots();
    juce::JUCEApplicationBase::quit();
}

void SnapshotDriver::playNote (double seconds)
{
    auto& p = editor.getProcessor();
    constexpr double rate = 48000.0;
    constexpr int chunk = 256;
    float targets[PatchPlayer::kNumMacros];
    for (int i = 0; i < PatchPlayer::kNumMacros; ++i)
        targets[i] = p.apvts.getRawParameterValue ("macro" + juce::String (i + 1))->load();

    if (program == nullptr)
    {
        std::string error;
        program = PatchProgram::build (p.getUiPatch(), rate, error);
        if (program == nullptr)
        {
            std::printf ("snapshot: patch did not compile: %s\n", error.c_str());
            return;
        }
        player = std::make_unique<PatchPlayer>();
        player->prepare (rate, chunk, targets);
        player->setProgram (program.get());
        player->setHostTransport (140.0, false, false, 0.0);
        player->setVoiceControls (PatchPlayer::VoiceMode::Patch, 0.0);
    }

    std::vector<NoEvent> none;
    if (! noteStarted)
    {
        // One chunk first: the voice mode (Reese is mono) takes effect at a
        // chunk start, and a mode change releases held notes.
        auto it = none.cbegin();
        player->renderChunk (chunk, targets, it, none.cend(), 0);
        const uint8_t noteOn[3] = { 0x90, 40, 100 };
        player->handleMidi (noteOn, 3);
        noteStarted = true;
    }
    const int total = (int) (seconds * rate);
    float peakL = 0.0f, peakR = 0.0f;
    for (int done = 0; done < total; done += chunk)
    {
        auto it = none.cbegin();
        player->renderChunk (chunk, targets, it, none.cend(), 0);
        if (done + chunk >= total - 4800) // the last 100 ms
            for (int i = 0; i < chunk; ++i)
            {
                peakL = juce::jmax (peakL, std::abs (player->outL()[i]));
                peakR = juce::jmax (peakR, std::abs (player->outR()[i]));
            }
        if (done + chunk < total)
        {
            NodeMeterBank discard;
            player->harvestMeters (discard); // keep only the last chunk's peaks
        }
    }
    player->harvestMeters (p.nodeMeters());
    p.publishOutputPeak (0, peakL);
    p.publishOutputPeak (1, peakR);
    editor.tickMeters();
}

void SnapshotDriver::shoot (const juce::String& name, float pixelScale)
{
    const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, pixelScale);
    const auto file = dir.getChildFile (name + ".png");
    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat png;
    const bool ok = out.openedOk() && png.writeImageToStream (image, out);
    std::printf ("snapshot: %s %s (%d x %d)\n", ok ? "wrote" : "FAILED", file.getFullPathName().toRawUTF8(),
                 image.getWidth(), image.getHeight());
    std::fflush (stdout);
}

// Every built-in through the real node view: its probed node list and its
// own edges, one image each, and a printed line saying whether any name is
// cut off, any pills overlap or any pill leaves the view.
void SnapshotDriver::checkNodeViews()
{
    const auto folder = dir.getChildFile ("nodes");
    folder.createDirectory();
    auto& view = editor.nodeView();
    int bad = 0;
    for (int i = 0; i < numBuiltinPresets(); ++i)
    {
        const auto parsed = parsePatch (builtinPresetText (i));
        if (! parsed.ok())
            continue;
        const auto touched = probeTouchedNodes (parsed.patch, 48000.0);
        view.setGraph (touched, true, parsed.patch.graphJson);
        const auto problems = view.placementProblems();
        bad += problems.empty() ? 0 : 1;
        std::string list;
        for (const auto& p : problems)
            list += (list.empty() ? "" : "; ") + p;
        const juce::String file (juce::File (builtinPresetFile (i)).getFileNameWithoutExtension());
        std::printf ("nodecheck: %-16s %2d nodes, font %.1f pt: %s\n", file.toRawUTF8(), view.numNodes(),
                     view.pillFont(), list.empty() ? "all names fit" : list.c_str());
        const auto image = view.createComponentSnapshot (view.getLocalBounds(), true, 1.0f);
        const auto out = folder.getChildFile (file + ".png");
        out.deleteFile();
        juce::FileOutputStream stream (out);
        juce::PNGImageFormat png;
        if (stream.openedOk())
            png.writeImageToStream (image, stream);
    }
    std::printf ("nodecheck: %d of %d built-ins have a problem\n", bad, numBuiltinPresets());
    std::fflush (stdout);
}

void SnapshotDriver::runShots()
{
    const int w = Theme::baseWidth, h = Theme::baseHeight;
    editor.setSize (w, h);
    editor.showPage (0);

    editor.setKeyOverride (true);  // the shots don't depend on this machine's key
    playNote (0.6);
    shoot ("default");
    playNote (0.1);
    shoot ("default@2x", 2.0f);

    editor.setPromptText (kPrompt);
    HeaderState gen;
    gen.generating = true;
    gen.canGenerate = true;
    gen.kind = HeaderState::Kind::Busy;
    gen.stage = "Waiting for model";
    gen.seconds = 7;
    editor.setHeaderOverride (gen);
    playNote (0.1);
    shoot ("generating");

    HeaderState err;
    err.canGenerate = true;
    err.kind = HeaderState::Kind::Error;
    err.text = "Generation failed: too loud: peak +14.2 dBFS (max +12) at note 28, macros all 1";
    err.detail = "too loud: peak +14.2 dBFS (max +12) at note 28, macros all 1 (after 2 retries; the last "
                 "attempt's lint and compile passed, the offline test rejected it).\n\nThe current patch is "
                 "still playing.";
    editor.setHeaderOverride (err);
    editor.setErrorDetailOpen (true);
    shoot ("error");
    editor.setErrorDetailOpen (false);
    editor.setHeaderOverride (std::nullopt);
    editor.setPromptText ({});

    HeaderState noKey;
    noKey.kind = HeaderState::Kind::Info;
    noKey.text = "Loaded Reese";
    noKey.canGenerate = false;
    editor.setHeaderOverride (noKey);
    shoot ("nokey");
    editor.setHeaderOverride (std::nullopt);

    editor.showPage (1);
    shoot ("code");
    editor.showPage (0);
    shoot ("presets");
    editor.showPage (2);
    shoot ("history");
    editor.showPage (0);

    editor.setSize (w * 3 / 4, h * 3 / 4);
    playNote (0.1);
    shoot ("scale_075");
    editor.setSize (w * 3 / 2, h * 3 / 2);
    playNote (0.1);
    shoot ("scale_150");
    editor.setSize (w, h);

    // Last: closing Settings leaves keyboard focus on the gear.
    using KeyTest = VibeSynthAudioProcessor::KeyTest;
    editor.showSettingsForSnapshot ("sk-snapshot-not-a-real-key", { KeyTest::State::Ok, "Key works" });
    shoot ("settings_key");
    editor.showSettingsForSnapshot ("sk-snapshot-not-a-real-key",
                                    { KeyTest::State::Failed, juce::String (keyTestMessage (MetaClient::interpret (401, "", "", ""))) });
    shoot ("settings_key_error");
    editor.closeSettings();

    runM11Snapshots (editor, [this] (const juce::String& n) { shoot (n); });  // M11
    checkNodeViews();
}

} // namespace vibesynth::ui
