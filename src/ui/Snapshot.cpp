// M9 design review snapshots. See Snapshot.h.

#include "ui/Snapshot.h"

#include <cstdio>
#include <utility>

#include "PluginEditor.h"
#include "ai/Generator.h"
#include "ui/SnapshotM11.h"
#include "ui/SnapshotM13.h"
#include "ai/MetaClient.h"
#include "patch/PatchPlayer.h"
#include "patch/PatchTweaks.h"
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
        // M10: the processor's tweaks and traces, as the audio thread uses them.
        player->setNodeHooks (&p.nodeOverrides(), &p.nodeTraces());
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

void SnapshotDriver::retrigger (double releaseSeconds)
{
    if (player == nullptr)
        return;
    const uint8_t noteOff[3] = { 0x80, 40, 0 };
    player->handleMidi (noteOff, 3);
    playNote (releaseSeconds);
    const uint8_t noteOn[3] = { 0x90, 40, 100 };
    player->handleMidi (noteOn, 3);
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

    // M10 inspector: Reese's filter with a cutoff offset and its resonance
    // mapped to the unused macro 5, a bypassed fold, then an envelope, an
    // oscillator and the map-to-macro menu.
    using vibesynth::NodeRef;
    using vibesynth::RackKind;
    auto& proc = editor.getProcessor();
    proc.setNodeArg ("flt_lp:0", "cutoff", vibesynth::ArgTweak::kOffset, 0.12);
    proc.setNodeMacroMap ("flt_lp:0", "res", 5, 0.1, 0.6);
    proc.setNodeBypass ("fx_fold#0", true);
    editor.setKeyOverride (true);  // refresh() now: no message loop runs between shots
    playNote (0.4);
    editor.openInspector (NodeRef { RackKind::Voice, ModuleId::FltLp, 0 });
    playNote (0.3);
    shoot ("inspector_flt_lp");
    // Release, then a new note: the rolling trace shows the release tail,
    // the attack, the decay and the sustain.
    editor.openInspector (NodeRef { RackKind::Voice, ModuleId::EnvAdsr, 0 });
    retrigger (0.6);
    for (int i = 0; i < 12; ++i)
        playNote (0.1);
    shoot ("inspector_env_adsr");
    editor.openInspector (NodeRef { RackKind::Voice, ModuleId::OscSupersaw, 0 });
    playNote (0.2);
    shoot ("inspector_osc");
    editor.openInspector (NodeRef { RackKind::Voice, ModuleId::FltLp, 0 });
    playNote (0.1);
    editor.nodeInspector().showMapMenu (1);
    shoot ("inspector_map_menu");
    juce::PopupMenu::dismissAllActiveMenus();
    editor.closeInspector();
    proc.setNodeArg ("flt_lp:0", "cutoff", 0, 0.0);
    proc.clearNodeMacroMap ("flt_lp:0", "res");
    proc.setNodeBypass ("fx_fold#0", false);
    editor.setKeyOverride (true);

    // Last: closing Settings leaves keyboard focus on the gear.
    using KeyTest = VibeSynthAudioProcessor::KeyTest;
    editor.showSettingsForSnapshot ("sk-snapshot-not-a-real-key", { KeyTest::State::Ok, "Key works" });
    shoot ("settings_key");
    editor.showSettingsForSnapshot ("sk-snapshot-not-a-real-key",
                                    { KeyTest::State::Failed, juce::String (keyTestMessage (MetaClient::interpret (401, "", "", ""))) });
    shoot ("settings_key_error");
    editor.closeSettings();

    runM11Snapshots (editor, [this] (const juce::String& n) { shoot (n); }, [this] (double t) { playNote (t); });

    // M13 shots run on Reese (left loaded by the M11 shots), before M12 swaps the patch.
    runM13Snapshots (editor, [this] (const juce::String& n) { shoot (n); }, [this] (double t) { playNote (t); });

    // M12 inspector snapshots: osc_wt (table choice + pos), osc_fm, flt_formant.
    {
        const auto m12File = dir.getChildFile ("_m12_temp.pulsar");
        vibesynth::Patch m12Patch;
        m12Patch.name = "M12 Growl";
        m12Patch.prompt = "wavetable and FM formant bass";
        m12Patch.voiceMode = "mono";
        m12Patch.trim = 0.75;
        m12Patch.macros = {
            { "Morph", 0.45 }, { "Growl", 0.5 }, { "FM", 0.4 }, { "Vowel", 0.35 },
            { "Resonance", 0.6 }, { "Rate", 0.5 }, { "Space", 0.25 }, { "Drive", 0.35 }
        };
        m12Patch.init = "0;";
        m12Patch.voice =
            "lfo = mod_lfo(0, 1.0, 0.0);\n"
            "wt = osc_wt(0, freq, 2, 0.2 + m1 * 0.5 + lfo * 0.2);\n"
            "fm = osc_fm(0, freq, 0.25, 0.2 + m3 * 0.5, 0.15);\n"
            "sig = fx_shape(wt * 0.6 + fm * 0.4, 0.2 + m2 * 0.4, 0.25);\n"
            "sig = flt_formant(0, sig, 0.2 + m4 * 0.5 + lfo * 0.2, 0.3 + m5 * 0.5);\n"
            "env = env_adsr(0, gate, 0.02, 0.3, 0.75, 0.2);\n"
            "out = sig * env * vel * 0.4;";
        m12Patch.master =
            "d = 0.15 + m8 * 0.3;\n"
            "outl = bus_sat(inl, d);\n"
            "outr = bus_sat(inr, d);";
        m12File.replaceWithText (vibesynth::serializePatch (m12Patch));
        const int prevVer = proc.getPatchVersion();
        proc.loadPatchFile (m12File);
        for (int i = 0; i < 500 && (proc.getPatchVersion() == prevVer || ! proc.areNodesKnown()); ++i)
        {
            juce::Thread::sleep (10);
            proc.deliverPendingResults();
        }
        m12File.deleteFile();
        program.reset();
        player.reset();
        noteStarted = false;
        proc.setNodeArg ("osc_wt:0", "table", vibesynth::ArgTweak::kReplace, 2.0);
        editor.setKeyOverride (true);
        playNote (0.4);
        // Each at 1x, 0.75x and 1.5x (overlap review at every scale).
        const std::pair<ModuleId, const char*> m12Shots[] = { { ModuleId::OscWt, "inspector_osc_wt" },
                                                              { ModuleId::OscFm, "inspector_osc_fm" },
                                                              { ModuleId::FltFormant, "inspector_flt_formant" } };
        for (const auto& [module, name] : m12Shots)
        {
            editor.setSize (w, h);
            editor.openInspector (NodeRef { RackKind::Voice, module, 0 });
            playNote (0.2);
            shoot (name);
            editor.setSize (w * 3 / 4, h * 3 / 4);
            playNote (0.1);
            shoot (juce::String (name) + "_075");
            editor.setSize (w * 3 / 2, h * 3 / 2);
            playNote (0.1);
            shoot (juce::String (name) + "_150");
        }
        editor.setSize (w, h);
        editor.closeInspector();
    }

    // M12 Tier 2 inspector snapshots: fx_phaser (voice), master/bus_delay
    // (a Beats knob) and master/bus_width, each at 1x, 0.75x and 1.5x.
    {
        const auto t2File = dir.getChildFile ("_m12t2_temp.pulsar");
        vibesynth::Patch t2Patch;
        t2Patch.name = "Phased Echo Pad";
        t2Patch.prompt = "wide phased pad with ping-pong echoes";
        t2Patch.voiceMode = "poly";
        t2Patch.trim = 0.75;
        t2Patch.macros = {
            { "Sweep", 0.5 }, { "Depth", 0.7 }, { "Feedback", 0.4 }, { "Shift", 0.5 },
            { "Echo", 0.3 }, { "Repeats", 0.4 }, { "Width", 0.7 }, { "Bright", 0.5 }
        };
        t2Patch.init = "0;";
        t2Patch.voice =
            "lfo = mod_lfo(0, 4.0, 0.0);\n"
            "s = osc_saw(0, freq) * 0.5 + osc_saw(1, freq * 1.006) * 0.5;\n"
            "s = fx_phaser(0, s, 0.2 + lfo * 0.6 * m1, m2, m3);\n"
            "s = fx_fshift(0, s, 0.5 + (m4 - 0.5) * 0.2);\n"
            "s = flt_lp(0, s, 0.5 + m8 * 0.4, 0.2);\n"
            "env = env_adsr(0, gate, 0.4, 0.5, 0.8, 0.6);\n"
            "out = s * env * vel * 0.35;";
        t2Patch.master =
            "d = bus_delay(inl, inr, 0.75, m6 * 0.8, m5 * 0.5);\n"
            "dr = bus_delay_r();\n"
            "outl = bus_width(d, dr, m7);\n"
            "outr = bus_width_r();";
        t2File.replaceWithText (vibesynth::serializePatch (t2Patch));
        const int prevVer = proc.getPatchVersion();
        proc.loadPatchFile (t2File);
        for (int i = 0; i < 500 && (proc.getPatchVersion() == prevVer || ! proc.areNodesKnown()); ++i)
        {
            juce::Thread::sleep (10);
            proc.deliverPendingResults();
        }
        t2File.deleteFile();
        program.reset();
        player.reset();
        noteStarted = false;
        editor.setKeyOverride (true);
        playNote (0.6);
        const std::pair<NodeRef, const char*> t2Shots[] = {
            { NodeRef { RackKind::Voice, ModuleId::FxPhaser, 0 }, "inspector_fx_phaser" },
            { NodeRef { RackKind::Master, ModuleId::BusDelay, 0 }, "inspector_bus_delay" },
            { NodeRef { RackKind::Master, ModuleId::BusWidth, 0 }, "inspector_bus_width" }
        };
        for (const auto& [node, name] : t2Shots)
        {
            editor.setSize (w, h);
            editor.openInspector (node);
            playNote (0.3);
            shoot (name);
            editor.setSize (w * 3 / 4, h * 3 / 4);
            playNote (0.1);
            shoot (juce::String (name) + "_075");
            editor.setSize (w * 3 / 2, h * 3 / 2);
            playNote (0.1);
            shoot (juce::String (name) + "_150");
        }
        editor.setSize (w, h);
        editor.closeInspector();
    }

    checkNodeViews();
}

} // namespace vibesynth::ui
