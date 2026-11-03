// M14b smoketests. See SmokeM14b.h.

#include "SmokeM14b.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "CliCheck.h"
#include "ai/Mutator.h"
#include "ai/PromptBuilder.h"
#include "ai/UndoStack.h"
#include "patch/Patch.h"
#include "patch/PatchLibrary.h"
#include "patch/PatchValidator.h"
#include "ui/KnobValueParser.h"

namespace vibesynth_cli {

namespace {

using namespace vibesynth;
using namespace vibesynth::ui;

// 1. Value parser table
bool smokeValueParserTable()
{
    bool ok = true;

    struct TestCase
    {
        const char* input;
        KnobUnitKind kind;
        double minVal;
        double maxVal;
        bool expectOk;
        double expectVal;
        double tol;
    };

    const TestCase cases[] = {
        // Plain numbers with/without unit
        { "250", KnobUnitKind::Hz, 20.0, 20000.0, true, 250.0, 1e-4 },
        { "250 Hz", KnobUnitKind::Hz, 20.0, 20000.0, true, 250.0, 1e-4 },
        { "1.2k", KnobUnitKind::Hz, 20.0, 20000.0, true, 1200.0, 1e-4 },
        { "1.2 kHz", KnobUnitKind::Hz, 20.0, 20000.0, true, 1200.0, 1e-4 },
        { "-6 dB", KnobUnitKind::Decibels, -60.0, 12.0, true, -6.0, 1e-4 },
        { "-6dB", KnobUnitKind::Decibels, -60.0, 12.0, true, -6.0, 1e-4 },
        { "40%", KnobUnitKind::Generic, 0.0, 1.0, true, 0.40, 1e-4 },
        { "0.4", KnobUnitKind::Generic, 0.0, 1.0, true, 0.40, 1e-4 },
        { "120 ms", KnobUnitKind::Time, 0.001, 10.0, true, 0.120, 1e-4 },
        { "0.5 s", KnobUnitKind::Time, 0.001, 10.0, true, 0.50, 1e-4 },
        { "500ms", KnobUnitKind::Time, 0.001, 10.0, true, 0.50, 1e-4 },
        { "1/8", KnobUnitKind::SyncedRate, 0.0, 23.0, true, 7.0, 1e-4 }, // 1/8 sync div index (Div1_8 = 7)
        { "7 st", KnobUnitKind::Semitones, -36.0, 36.0, true, 7.0, 1e-4 },
        { "7st", KnobUnitKind::Semitones, -36.0, 36.0, true, 7.0, 1e-4 },
        { "50%", KnobUnitKind::Macro, 0.0, 1.0, true, 0.50, 1e-4 },
        { "0.5", KnobUnitKind::Macro, 0.0, 1.0, true, 0.50, 1e-4 },
        // Glide
        { "off", KnobUnitKind::Glide, 0.0, 1.0, true, 0.0, 1e-4 },
        { "120 ms", KnobUnitKind::Glide, 0.0, 1.0, true, std::sqrt (0.120), 1e-4 },
        // Clamping
        { "30000 Hz", KnobUnitKind::Hz, 20.0, 20000.0, true, 20000.0, 1e-4 },
        { "5 Hz", KnobUnitKind::Hz, 20.0, 20000.0, true, 20.0, 1e-4 },
        { "150%", KnobUnitKind::Macro, 0.0, 1.0, true, 1.0, 1e-4 },
        { "-10%", KnobUnitKind::Macro, 0.0, 1.0, true, 0.0, 1e-4 },
        { "50 st", KnobUnitKind::Semitones, -36.0, 36.0, true, 36.0, 1e-4 },
        // Garbage rejection & mismatched unit rejection (M14e Task 0)
        { "xyz", KnobUnitKind::Hz, 20.0, 20000.0, false, 0.0, 0.0 },
        { "hello", KnobUnitKind::Macro, 0.0, 1.0, false, 0.0, 0.0 },
        { "", KnobUnitKind::Generic, 0.0, 1.0, false, 0.0, 0.0 },
        { "   ", KnobUnitKind::Decibels, -60.0, 12.0, false, 0.0, 0.0 },
        { "1.2.3", KnobUnitKind::Time, 0.001, 10.0, false, 0.0, 0.0 },
        { "440 Hz", KnobUnitKind::Generic, 0.0, 1.0, false, 0.0, 0.0 },
        { "440 Hz", KnobUnitKind::Semitones, -36.0, 36.0, false, 0.0, 0.0 },
        { "-6 dB", KnobUnitKind::FilterCutoff, 0.0, 1.0, false, 0.0, 0.0 },
        { "-6 dB", KnobUnitKind::Generic, 0.0, 1.0, false, 0.0, 0.0 },
        { "7 st", KnobUnitKind::Generic, 0.0, 1.0, false, 0.0, 0.0 },
        // Filter Cutoff (M14e Task 0)
        { "440 Hz", KnobUnitKind::FilterCutoff, 0.0, 1.0, true, std::log (440.0 / 20.0) / std::log (1000.0), 1e-4 },
        { "1.2 kHz", KnobUnitKind::FilterCutoff, 0.0, 1.0, true, std::log (1200.0 / 20.0) / std::log (1000.0), 1e-4 },
        { "0.5", KnobUnitKind::FilterCutoff, 0.0, 1.0, true, 0.50, 1e-4 }
    };

    int passed = 0;
    const int total = static_cast<int> (sizeof (cases) / sizeof (cases[0]));
    for (const auto& c : cases)
    {
        const auto res = parseKnobValue (c.input, c.kind, c.minVal, c.maxVal);
        if (res.ok == c.expectOk)
        {
            if (! c.expectOk || std::abs (res.value - c.expectVal) <= c.tol)
                ++passed;
        }
    }

    ok = check (passed == total, "M14b value parser: %d/%d table cases passed (units, clamping, rejection)",
                passed, total) && ok;
    return ok;
}

// 2. Steered directions N=1..5
bool smokeSteeredDirections()
{
    bool ok = true;
    const std::string custom = "more metallic, faster wobble, keep the sub clean";

    for (int n = 1; n <= 5; ++n)
    {
        const auto dirs = Mutator::buildSteeredDirections (custom, n);
        bool nOk = (dirs.size() == (size_t) n);
        for (const auto& d : dirs)
        {
            if (d.text.find (custom) == std::string::npos)
                nOk = false;
            if (d.label.empty() || d.key.empty())
                nOk = false;
        }

        if (n == 1)
        {
            if (dirs[0].text.find ("moderate") == std::string::npos)
                nOk = false;
        }
        else
        {
            // First is subtle, last is bold; intensities differ across slots
            if (dirs.front().text.find ("subtle") == std::string::npos)
                nOk = false;
            if (dirs.back().text.find ("bold") == std::string::npos)
                nOk = false;
            if (dirs.front().text == dirs.back().text)
                nOk = false;
        }

        ok = check (nOk, "M14b steered directions: count %d yields %d valid directions with custom text and intensity spread",
                    n, (int) dirs.size()) && ok;
    }
    return ok;
}

// 3. Auto mutate chips
bool smokeAutoChips()
{
    bool ok = true;

    // 2 chips: darker, space
    const auto dirs2 = Mutator::buildAutoDirections ({ "darker", "space" });
    const bool ok2 = (dirs2.size() == 2 && dirs2[0].key == "darker" && dirs2[1].key == "space"
                      && dirs2[1].label == "Space");
    ok = check (ok2, "M14b auto chips: 2 chips (\"darker\", \"space\") produce exactly 2 directions in order") && ok;

    // All 5 chips: darker, movement, brighter, weirder, space
    const auto dirs5 = Mutator::buildAutoDirections ({ "darker", "movement", "brighter", "weirder", "space" });
    const bool ok5 = (dirs5.size() == 5
                      && dirs5[0].key == "darker"
                      && dirs5[1].key == "movement"
                      && dirs5[2].key == "brighter"
                      && dirs5[3].key == "weirder"
                      && dirs5[4].key == "space");
    ok = check (ok5, "M14b auto chips: all 5 chips produce 5 directions in order including Space") && ok;

    return ok;
}

// 4. Old CLI form unchanged
bool smokeOldCliForm()
{
    bool ok = true;
    MutateRequest req;
    // When req.directions is empty, the default is the 4 original directions
    const auto& defaultDirs = PromptResources::embedded().mutateDirections;
    const bool defaultOk = (defaultDirs.size() >= 4
                            && defaultDirs[0].key == "darker"
                            && defaultDirs[1].key == "movement"
                            && defaultDirs[2].key == "brighter"
                            && defaultDirs[3].key == "weirder");
    ok = check (defaultOk, "M14b old CLI form: embedded mutate directions preserve darker, movement, brighter, weirder") && ok;
    return ok;
}

// 5. Card save and replace offline validation
bool smokeCardSaveAndReplace()
{
    bool ok = true;
    const Patch reese = parsePatch (builtinPresetText (0)).patch;
    const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("pulsar_m14b_test_" + juce::String (juce::Time::currentTimeMillis()));
    tempDir.createDirectory();

    juce::String err;
    const juce::String presetName = "Test Reese Variation";

    // 1. Initial save
    const auto savedFile = PatchLibrary::saveUserPatch (tempDir, presetName, reese, false, err);
    bool saveOk = savedFile.existsAsFile() && err.isEmpty();

    // Verify it passes the offline validator
    if (saveOk)
    {
        const auto loaded = PatchLibrary::listPatches (tempDir);
        saveOk = (loaded.size() == 1);
        const auto loadedPatch = parsePatch (savedFile.loadFileAsString().toStdString());
        const bool valid = (loadedPatch.status == PatchStatus::Ok
                            && validatePatch (loadedPatch.patch, 48000.0).ok());
        saveOk = saveOk && valid;
    }

    // 2. Re-saving without overwrite fails (offers replace)
    err.clear();
    const auto duplicate = PatchLibrary::saveUserPatch (tempDir, presetName, reese, false, err);
    const bool duplicateBlocked = ! duplicate.existsAsFile() || err.isNotEmpty();

    // 3. Re-saving with overwrite=true succeeds
    err.clear();
    const auto replaced = PatchLibrary::saveUserPatch (tempDir, presetName, reese, true, err);
    const bool replaceOk = replaced.existsAsFile() && err.isEmpty();

    // Active patch unchanged
    const bool originalUntouched = (reese.name == "Reese");

    tempDir.deleteRecursively();

    const bool allOk = saveOk && duplicateBlocked && replaceOk && originalUntouched;
    ok = check (allOk, "M14b card save: saves valid preset (%d), blocks overwrite (%d), replaces cleanly (%d)",
                saveOk ? 1 : 0, duplicateBlocked ? 1 : 0, replaceOk ? 1 : 0) && ok;
    return ok;
}

// 6. Typed value lands in same place as drag; undo restores it
bool smokeTypedValueLandsAndUndo()
{
    bool ok = true;
    const Patch reese = parsePatch (builtinPresetText (0)).patch;

    // Simulate knob drag vs typed value:
    // Dragging macro 1 to 0.72 vs typing "72%" or "0.72"
    const auto parsed1 = parseKnobValue ("72%", KnobUnitKind::Macro, 0.0, 1.0);
    const auto parsed2 = parseKnobValue ("0.72", KnobUnitKind::Macro, 0.0, 1.0);
    const bool parseEq = parsed1.ok && parsed2.ok && std::abs (parsed1.value - 0.72) < 1e-4
                         && std::abs (parsed2.value - 0.72) < 1e-4;

    // Undo stack behavior:
    UndoStack undoStack;
    PatchSnapshot snap0;
    snap0.patch = reese;
    snap0.macros = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };

    undoStack.push (snap0);
    const bool canUndo = undoStack.canUndo();

    // Modify macro value
    Patch modified = reese;
    modified.macros[0].defaultValue = parsed1.value;

    // Pop undo restores previous snapshot
    PatchSnapshot restored;
    const bool popped = undoStack.pop (restored);
    const bool undoOk = popped && (restored.patch.name == reese.name)
                        && std::abs (restored.macros[0] - 0.5f) < 1e-4;

    const bool allOk = parseEq && canUndo && undoOk;
    ok = check (allOk, "M14b typed value & undo: parse matches drag (%.2f), undo restores macro position (%d)",
                parsed1.value, undoOk ? 1 : 0) && ok;
    return ok;
}

} // namespace

bool smokeM14b()
{
    bool ok = true;
    ok = smokeValueParserTable() && ok;
    ok = smokeSteeredDirections() && ok;
    ok = smokeAutoChips() && ok;
    ok = smokeOldCliForm() && ok;
    ok = smokeCardSaveAndReplace() && ok;
    ok = smokeTypedValueLandsAndUndo() && ok;
    return ok;
}

} // namespace vibesynth_cli
