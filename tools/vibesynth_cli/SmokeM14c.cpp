// M14c smoketests. See SmokeM14c.h.

#include "SmokeM14c.h"

#include <cstdio>
#include <string>
#include <vector>

#include "Bench.h"
#include "CliCheck.h"
#include "Judge.h"
#include "Render.h"
#include "ai/AiSettings.h"
#include "ai/MetaClient.h"
#include "ai/PromptBuilder.h"
#include "ai/ResponseParser.h"
#include "patch/LexiconLint.h"
#include "patch/Patch.h"
#include "patch/PatchValidator.h"

namespace vibesynth_cli {

namespace {

using namespace vibesynth;

bool smokeJudgeParserAndTransport()
{
    bool ok = true;

    // 1. Valid reply parsing
    int score = 0;
    std::string reason;
    bool parsed = parseJudgeReply (R"({"score": 4, "reason": "Good punchy square bass."})", score, reason);
    ok = check (parsed && score == 4 && reason == "Good punchy square bass.",
                "M14c judge parses clean JSON reply") && ok;

    // Markdown fence wrapped
    parsed = parseJudgeReply ("```json\n{\"score\": 5, \"reason\": \"Exact match for prompt.\"}\n```", score, reason);
    ok = check (parsed && score == 5 && reason == "Exact match for prompt.",
                "M14c judge parses code-fenced JSON reply") && ok;

    // Out of range score
    parsed = parseJudgeReply (R"({"score": 6, "reason": "Too high."})", score, reason);
    ok = check (! parsed, "M14c judge rejects score > 5") && ok;

    parsed = parseJudgeReply (R"({"score": 0, "reason": "Too low."})", score, reason);
    ok = check (! parsed, "M14c judge rejects score < 1") && ok;

    // Missing reason
    parsed = parseJudgeReply (R"({"score": 4})", score, reason);
    ok = check (! parsed, "M14c judge rejects JSON missing reason") && ok;

    // Garbage text
    parsed = parseJudgeReply ("I think this patch is pretty good, 4 out of 5", score, reason);
    ok = check (! parsed, "M14c judge rejects non-JSON text") && ok;

    // 2. Fake transport: good reply on first attempt
    Patch testPatch;
    testPatch.name = "Test Square Bass";
    testPatch.voiceMode = "mono";
    testPatch.voice = "out = osc_square(0, freq) + osc_sine(0, freq);";
    AudioDescriptors desc;
    desc.brightnessHz = 800.0;
    desc.noisiness = 0.05;
    desc.movementPerSec = 1.0;
    desc.attackMs = 10.0;
    desc.subLevelPct = 40.0;
    desc.stereoWidth = 0.0;
    OscAnalysis osc = analyzeVoiceOscillators (testPatch.voice);
    AiConfig cfg;

    int callCount = 0;
    auto goodTransport = [&] (const ChatRequest&) -> ChatReply {
        ++callCount;
        ChatReply r;
        r.error = ChatError::None;
        r.content = R"({"score": 5, "reason": "Matches prompt accurately."})";
        r.promptTokens = 120;
        r.completionTokens = 20;
        return r;
    };

    JudgeResult jr = runJudge ("square bass", testPatch, desc, osc, cfg, "key", goodTransport);
    ok = check (jr.score == 5 && callCount == 1 && jr.reason == "Matches prompt accurately.",
                "M14c judge succeeds on first try with valid transport") && ok;

    // 3. Fake transport: garbage first try, valid second try (retry once)
    callCount = 0;
    auto retryTransport = [&] (const ChatRequest&) -> ChatReply {
        ++callCount;
        ChatReply r;
        r.error = ChatError::None;
        if (callCount == 1)
            r.content = "Sure! Here is my rating: 4/5 because it sounds great.";
        else
            r.content = R"({"score": 4, "reason": "Recovered valid score on retry."})";
        r.promptTokens = 150;
        r.completionTokens = 25;
        return r;
    };

    jr = runJudge ("square bass", testPatch, desc, osc, cfg, "key", retryTransport);
    ok = check (jr.score == 4 && callCount == 2 && jr.reason == "Recovered valid score on retry.",
                "M14c judge retries once on garbage and parses valid second reply") && ok;

    // 4. Fake transport: garbage on both tries -> records "no score"
    callCount = 0;
    auto garbageTransport = [&] (const ChatRequest&) -> ChatReply {
        ++callCount;
        ChatReply r;
        r.error = ChatError::None;
        r.content = "Still not JSON!";
        return r;
    };

    jr = runJudge ("square bass", testPatch, desc, osc, cfg, "key", garbageTransport);
    ok = check (jr.score == 0 && callCount == 2 && jr.reason == "no score" && jr.malformed,
                "M14c judge records 'no score' after failed retry on garbage") && ok;

    return ok;
}

bool smokeOscAnalysis()
{
    bool ok = true;

    // Snarl Reese: osc_supersaw + osc_saw + osc_sine -> isReeseLike = true
    const std::string reeseCode = R"(
        a = osc_supersaw(0, freq, 0.4, 0.65);
        b = osc_saw(0, freq * 0.99);
        out = (osc_sine(0, freq) * 0.5 + a * 0.5);
    )";
    OscAnalysis rOsc = analyzeVoiceOscillators (reeseCode);
    ok = check (rOsc.isReeseLike, "M14c osc analysis: supersaw + saw is reese-like") && ok;
    ok = check (rOsc.primaryOsc == "osc_supersaw", "M14c osc analysis: primary is osc_supersaw") && ok;
    ok = check (rOsc.callCounts["osc_supersaw"] == 1 && rOsc.callCounts["osc_saw"] == 1,
                "M14c osc analysis: correct call counts for reese") && ok;

    // Two detuned saws: osc_saw called twice -> isReeseLike = true
    const std::string twoSawsCode = R"(
        a = osc_saw(0, freq * 1.01);
        b = osc_saw(1, freq * 0.99);
        out = a + b;
    )";
    OscAnalysis sOsc = analyzeVoiceOscillators (twoSawsCode);
    ok = check (sOsc.isReeseLike, "M14c osc analysis: two detuned saws is reese-like") && ok;
    ok = check (sOsc.primaryOsc == "osc_saw", "M14c osc analysis: primary is osc_saw") && ok;

    // Square bass: osc_square + osc_sine -> NOT reese-like
    const std::string sqCode = R"(
        // comments with osc_supersaw() should be ignored
        /* block comment osc_saw() */
        sq = osc_square(0, freq, 0.5);
        sub = osc_sine(0, freq);
        out = sq * 0.7 + sub * 0.3;
    )";
    OscAnalysis sqOsc = analyzeVoiceOscillators (sqCode);
    ok = check (! sqOsc.isReeseLike, "M14c osc analysis: square bass is not reese-like") && ok;
    ok = check (sqOsc.primaryOsc == "osc_square", "M14c osc analysis: primary is osc_square") && ok;
    ok = check (sqOsc.callCounts.count ("osc_supersaw") == 0,
                "M14c osc analysis: comments are ignored during osc count") && ok;

    // FM bass
    const std::string fmCode = "out = osc_fm(0, freq, 2.0, 1.5);";
    OscAnalysis fmOsc = analyzeVoiceOscillators (fmCode);
    ok = check (! fmOsc.isReeseLike && fmOsc.primaryOsc == "osc_fm",
                "M14c osc analysis: FM bass identified as osc_fm and not reese-like") && ok;

    return ok;
}

bool smokeDescriptors()
{
    bool ok = true;

    // Generate a 1-second synthetic sine buffer at 440 Hz
    constexpr double sr = 48000.0;
    constexpr int nSamples = 48000;
    juce::AudioBuffer<float> buf (2, nSamples);
    buf.clear();
    for (int i = 0; i < nSamples; ++i)
    {
        const float s = std::sin (2.0f * 3.14159265f * 440.0f * (float) i / (float) sr);
        buf.setSample (0, i, s);
        buf.setSample (1, i, s); // mono
    }

    AudioDescriptors d = measureDescriptors (buf, sr);
    ok = check (d.brightnessHz >= 400.0 && d.brightnessHz <= 500.0,
                "M14c descriptors: 440 Hz sine brightness is ~440 Hz") && ok;
    ok = check (d.noisiness < 0.05,
                "M14c descriptors: pure sine has very low flatness") && ok;
    ok = check (d.stereoWidth == 0.0,
                "M14c descriptors: dual-mono buffer has 0.0 stereo width") && ok;

    return ok;
}

bool smokeExplainPromptTable()
{
    bool ok = true;
    const PromptBuilder b;

    auto hasRecipe = [] (const ExplainResult& r, const std::string& file) {
        for (const auto& it : r.recipes)
            if (it.file == file) return true;
        for (const auto& it : r.paddedRecipes)
            if (it.file == file) return true;
        return false;
    };
    auto hasExample = [] (const ExplainResult& r, const std::string& file) {
        for (const auto& it : r.examples)
            if (it.file == file) return true;
        return false;
    };

    // 1. "Square riddim bass"
    {
        const ExplainResult r = b.explainPrompt ("Square riddim bass");
        ok = check (r.examples.size() >= 2 && r.examples[0].category == "bass-square",
                    "explain 'Square riddim bass': slot 1 is bass-square (%s)", r.examples.empty() ? "" : r.examples[0].category.c_str()) && ok;
        ok = check (r.examples.size() >= 2 && r.examples[1].category != "bass-square",
                    "explain 'Square riddim bass': slot 2 is different category (%s)", r.examples.size() > 1 ? r.examples[1].category.c_str() : "") && ok;
        ok = check (hasRecipe (r, "square_pwm_bass.md"), "explain 'Square riddim bass': square/pwm recipe included") && ok;
        ok = check (! hasRecipe (r, "reese.md") && ! hasExample (r, "example_bass.md"),
                    "explain 'Square riddim bass': reese NOT included") && ok;
    }

    // 2. "industrial siren"
    {
        const ExplainResult r = b.explainPrompt ("industrial siren");
        ok = check (hasRecipe (r, "metal_siren.md"), "explain 'industrial siren': metal_siren recipe included") && ok;
        ok = check (r.examples.size() >= 1 && r.examples[0].category == "fx",
                    "explain 'industrial siren': fx example in slot 1 (%s)", r.examples.empty() ? "" : r.examples[0].category.c_str()) && ok;
        ok = check (! hasRecipe (r, "reese.md") && ! hasExample (r, "example_bass.md"),
                    "explain 'industrial siren': no reese") && ok;
    }

    // 3. "lush pad"
    {
        const ExplainResult r = b.explainPrompt ("lush pad");
        ok = check (r.examples.size() >= 1 && r.examples[0].category == "pad",
                    "explain 'lush pad': pad example in slot 1 (%s)", r.examples.empty() ? "" : r.examples[0].category.c_str()) && ok;
        ok = check (hasRecipe (r, "lush_pad.md"), "explain 'lush pad': lush_pad recipe included") && ok;
        ok = check (! hasRecipe (r, "ott_glue.md") && ! hasRecipe (r, "tempo_sync.md"),
                    "explain 'lush pad': no ott_glue or tempo_sync") && ok;
    }

    // 4. "neuro reese"
    {
        const ExplainResult r = b.explainPrompt ("neuro reese");
        ok = check (r.examples.size() >= 1 && r.examples[0].file == "example_bass.md",
                    "explain 'neuro reese': reese example in slot 1 (%s)", r.examples.empty() ? "" : r.examples[0].file.c_str()) && ok;
        ok = check (hasRecipe (r, "reese.md"), "explain 'neuro reese': reese recipe included") && ok;
    }

    // 5. "a sound"
    {
        const ExplainResult r = b.explainPrompt ("a sound");
        ok = check (r.examples.size() >= 2 && r.examples[0].file == "example_bloom_drift.md" && r.examples[1].file == "example_sine_wub.md",
                    "explain 'a sound': neutral pair (Bloom Drift + Sine Wave Wub)") && ok;
        ok = check (! hasRecipe (r, "reese.md") && ! hasRecipe (r, "tempo_sync.md"),
                    "explain 'a sound': no reese, no tempo_sync") && ok;
    }

    // 6. "music box melody that never repeats"
    {
        const ExplainResult r = b.explainPrompt ("music box melody that never repeats");
        ok = check (r.examples.size() >= 1 && r.examples[0].category == "generative",
                    "explain 'music box melody': generative example in slot 1 (%s)", r.examples.empty() ? "" : r.examples[0].category.c_str()) && ok;
        ok = check (hasRecipe (r, "generative_arp.md"), "explain 'music box melody': generative arp recipe included") && ok;
    }

    // 7. "lofi house chords"
    {
        const ExplainResult r = b.explainPrompt ("lofi house chords");
        ok = check (r.examples.size() >= 1 && r.examples[0].category == "chords",
                    "explain 'lofi house chords': chords example in slot 1 (%s)", r.examples.empty() ? "" : r.examples[0].category.c_str()) && ok;
        ok = check (hasRecipe (r, "one_finger_chords.md"), "explain 'lofi house chords': one-finger chords recipe included") && ok;
    }

    // 8. "ocean waves crashing on a beach"
    {
        const ExplainResult r = b.explainPrompt ("ocean waves crashing on a beach");
        ok = check (hasRecipe (r, "nature_fx.md"), "explain 'ocean waves': nature fx recipe included") && ok;
    }

    return ok;
}

bool smokeSelectionAndGatingRules()
{
    bool ok = true;
    const PromptBuilder b;

    const std::vector<std::string> nonReesePrompts = {
        "acid bass", "clean pluck", "808 sub kick", "warm analog organ", "punchy brass",
        "evolving ambient shimmer", "crystal glass bell", "vocal chop lead", "glitchy drum break", "nature wind"
    };

    for (const auto& prompt : nonReesePrompts)
    {
        const ExplainResult r = b.explainPrompt (prompt);
        bool hasReeseEx = false;
        for (const auto& e : r.examples)
            if (e.file == "example_bass.md") hasReeseEx = true;
        bool hasReeseRec = false;
        for (const auto& rec : r.recipes)
            if (rec.file == "reese.md") hasReeseRec = true;
        for (const auto& rec : r.paddedRecipes)
            if (rec.file == "reese.md") hasReeseRec = true;
        ok = check (! hasReeseEx && ! hasReeseRec,
                    "reese is never sent for non-reese prompt '%s'", prompt.c_str()) && ok;

        if (r.examples.size() >= 2)
        {
            ok = check (r.examples[0].category != r.examples[1].category,
                        "examples come from different categories for '%s' (%s vs %s)",
                        prompt.c_str(), r.examples[0].category.c_str(), r.examples[1].category.c_str()) && ok;
        }
    }

    // tempo_sync rule: only under movement words
    {
        const ExplainResult r1 = b.explainPrompt ("bright static bell");
        bool hasSync = false;
        for (const auto& it : r1.recipes) if (it.file == "tempo_sync.md") hasSync = true;
        for (const auto& it : r1.paddedRecipes) if (it.file == "tempo_sync.md") hasSync = true;
        ok = check (! hasSync, "tempo_sync excluded when prompt lacks movement words") && ok;

        const ExplainResult r2 = b.explainPrompt ("tempo sync trance gate");
        bool hasSync2 = false;
        for (const auto& it : r2.recipes) if (it.file == "tempo_sync.md") hasSync2 = true;
        for (const auto& it : r2.paddedRecipes) if (it.file == "tempo_sync.md") hasSync2 = true;
        ok = check (hasSync2, "tempo_sync included when movement words are present") && ok;
    }

    // ott_glue rule: only for bass prompts or explicit ott words
    {
        const ExplainResult r1 = b.explainPrompt ("crystal ambient choir");
        bool hasOtt = false;
        for (const auto& it : r1.recipes) if (it.file == "ott_glue.md") hasOtt = true;
        for (const auto& it : r1.paddedRecipes) if (it.file == "ott_glue.md") hasOtt = true;
        ok = check (! hasOtt, "ott_glue excluded for non-bass, non-ott prompt") && ok;

        const ExplainResult r2 = b.explainPrompt ("squash deep sub");
        bool hasOtt2 = false;
        for (const auto& it : r2.recipes) if (it.file == "ott_glue.md") hasOtt2 = true;
        for (const auto& it : r2.paddedRecipes) if (it.file == "ott_glue.md") hasOtt2 = true;
        ok = check (hasOtt2, "ott_glue included when ott words are present") && ok;
    }

    return ok;
}

bool smokeNewExamplesAndRecipes()
{
    bool ok = true;
    const PromptResources& res = PromptResources::embedded();
    const double kRate = 48000.0;

    static const char* kNewExamples[8] = {
        "example_howling_bass.md", "example_garage_chopper.md", "example_bloom_drift.md",
        "example_funky_lead.md", "example_sine_wub.md", "example_pentatonic_dice.md",
        "example_minor9_lush.md", "example_bloom_pulse.md"
    };

    for (const char* name : kNewExamples)
    {
        auto it = std::find_if (res.examples.begin(), res.examples.end(),
                                [&] (const auto& e) { return e.file == name; });
        if (! check (it != res.examples.end(), "M14c example %s is present", name))
        {
            ok = false;
            continue;
        }
        const ResponseParseResult pr = parseResponse (it->reply);
        ok = check (pr.ok() && pr.notes.empty(), "M14c example %s parses with no fix-ups%s%s", name,
                    pr.ok() ? "" : ": ", pr.error.c_str()) && ok;
        if (! pr.ok()) continue;
        Patch p = pr.patch;
        p.prompt = it->prompt;
        const ValidationResult vr = validatePatch (p, kRate);
        ok = check (vr.ok(), "M14c example %s passes offline test%s%s", name,
                    vr.ok() ? "" : ": ", vr.error.c_str()) && ok;
    }

    static const char* kNewRecipes[15] = {
        "square_pwm_bass.md", "sub_808_glide.md", "acid_bass.md", "hoover_stab.md",
        "clean_fm_wub.md", "noise_bass.md", "vibrato_lead.md", "lush_pad.md",
        "metal_siren.md", "generative_arp.md", "one_finger_chords.md",
        "evolving_shimmer_drone.md", "pulsing_pad.md", "lofi_keys.md", "nature_fx.md"
    };

    for (const char* name : kNewRecipes)
    {
        auto it = std::find_if (res.recipes.begin(), res.recipes.end(),
                                [&] (const auto& r) { return r.file == name; });
        if (! check (it != res.recipes.end(), "M14c recipe %s is present", name))
        {
            ok = false;
            continue;
        }
        ok = check (! it->category.empty(), "M14c recipe %s has a category (%s)", name, it->category.c_str()) && ok;
        const auto s = juce::String (it->text).fromFirstOccurrenceOf ("```eel", false, false)
                           .fromFirstOccurrenceOf ("\n", false, false);
        const std::string code = s.upToFirstOccurrenceOf ("```", false, false).toStdString();
        const bool master = code.find ("// master") == 0;
        const size_t split = master ? std::string::npos : code.find ("\n// master");
        LintResult lr = lintSection (code.substr (0, split), master ? "master" : "voice");
        if (split != std::string::npos)
        {
            const LintResult lm = lintSection (code.substr (split + 1), "master");
            lr.issues.insert (lr.issues.end(), lm.issues.begin(), lm.issues.end());
        }
        ok = check (lr.ok(), "M14c recipe %s lints clean%s%s", name, lr.ok() ? "" : ": ", lr.message().c_str()) && ok;
    }

    return ok;
}

bool smokeNewBuiltinsOfflineAndCost()
{
    bool ok = true;
    const double kRate = 48000.0;
    static const char* kNewBuiltins[8] = {
        "howling_bass.pulsar", "garage_chopper.pulsar", "bloom_drift.pulsar",
        "funky_sync_lead.pulsar", "sine_wave_wub.pulsar", "pentatonic_dice.pulsar",
        "minor9_lush.pulsar", "bloom_pulse.pulsar"
    };

    auto findBuiltin = [] (const std::string& name) -> Patch {
        for (int i = 0; i < numBuiltinPresets(); ++i)
            if (builtinPresetFile (i) == name)
                return parsePatch (builtinPresetText (i)).patch;
        return {};
    };

    for (const char* name : kNewBuiltins)
    {
        const Patch p = findBuiltin (name);
        ok = check (! p.name.empty(), "M14c builtin %s loads", name) && ok;
        const ValidationResult vr = validatePatch (p, kRate);
        ok = check (vr.ok(), "M14c builtin %s passes offline test%s%s", name,
                    vr.ok() ? "" : ": ", vr.error.c_str()) && ok;
        const PresetCost cost = measurePresetCost (p, 2, 24000, 2);
        const double voiceBudget = p.voiceBudgetNs();
        ok = check (cost.voiceNs <= voiceBudget * 1.5,
                    "M14c builtin %s voice cost %.0f ns (budget %.0f)", name, cost.voiceNs, voiceBudget) && ok;
        ok = check (cost.masterNs <= Patch::kMasterBudgetNs * 1.5,
                    "M14c builtin %s master cost %.0f ns (budget %.0f)", name, cost.masterNs, Patch::kMasterBudgetNs) && ok;
    }
    return ok;
}

bool smokeUserPresets()
{
    bool ok = true;
    const PromptBuilder b;
    const juce::File tempDir = juce::File::createTempFile ("pulsar_user_test_dir");
    tempDir.deleteFile();
    tempDir.createDirectory();

    // 1. Valid user patch with strong custom match
    Patch userP;
    userP.name = "Cyber Blaster";
    userP.prompt = "cybernetic laser blaster zap, high tech sci-fi beam";
    userP.voiceMode = "mono";
    userP.voice = "out = osc_saw(0, freq);";
    userP.macros.resize (8);
    for (int i = 0; i < 8; ++i)
    {
        userP.macros[i].label = "M" + std::to_string (i + 1);
        userP.macros[i].defaultValue = 0.5;
    }
    const juce::File validFile = tempDir.getChildFile ("cyber_blaster.pulsar");
    validFile.replaceWithText (serializePatch (userP));

    // 2. Broken patch
    const juce::File brokenFile = tempDir.getChildFile ("broken.pulsar");
    brokenFile.replaceWithText ("{ not valid json");

    // 3. Huge file (> 10 KB)
    const juce::File hugeFile = tempDir.getChildFile ("huge.pulsar");
    std::string hugePad (12000, ' ');
    userP.prompt = hugePad;
    hugeFile.replaceWithText (serializePatch (userP));

    // Load user presets from temp folder
    const auto userExamples = PromptBuilder::loadUserPresetExamples (tempDir.getFullPathName().toStdString());
    ok = check (userExamples.size() == 1 && userExamples[0].file == "cyber_blaster.pulsar",
                "loadUserPresetExamples loads valid file and skips broken and huge files (%d loaded)",
                (int) userExamples.size()) && ok;

    // Strong match takes slot 2
    {
        const ExplainResult r = b.explainPrompt ("cybernetic laser beam", userExamples);
        ok = check (r.examples.size() >= 2 && r.examples[1].file == "cyber_blaster.pulsar"
                        && r.examples[1].category == "user",
                    "user preset with strong match takes slot 2 (%s, %s)",
                    r.examples.size() > 1 ? r.examples[1].file.c_str() : "",
                    r.examples.size() > 1 ? r.examples[1].category.c_str() : "") && ok;
    }

    // Weak match does not take slot 2
    {
        const ExplainResult r = b.explainPrompt ("soft acoustic grand piano", userExamples);
        ok = check (r.examples.size() >= 2 && r.examples[1].file != "cyber_blaster.pulsar",
                    "user preset with weak match is NOT chosen for slot 2 (%s)",
                    r.examples.size() > 1 ? r.examples[1].file.c_str() : "") && ok;
    }

    // When setting is off, Generator never sends user presets
    {
        std::vector<PromptExample> emptyUserExamples;
        const ExplainResult r = b.explainPrompt ("cybernetic laser beam", emptyUserExamples);
        ok = check (r.examples.size() >= 2 && r.examples[1].category != "user",
                    "when user presets are not provided (setting off), none are sent (%s)",
                    r.examples.size() > 1 ? r.examples[1].category.c_str() : "") && ok;
    }

    tempDir.deleteRecursively();
    return ok;
}

} // namespace

bool smokeM14c()
{
    std::printf ("== M14c smoketests ==\n");
    bool ok = true;
    ok = smokeJudgeParserAndTransport() && ok;
    ok = smokeOscAnalysis() && ok;
    ok = smokeDescriptors() && ok;
    ok = smokeExplainPromptTable() && ok;
    ok = smokeSelectionAndGatingRules() && ok;
    ok = smokeNewExamplesAndRecipes() && ok;
    ok = smokeNewBuiltinsOfflineAndCost() && ok;
    ok = smokeUserPresets() && ok;
    return ok;
}

} // namespace vibesynth_cli
