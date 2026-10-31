// M10 smoketests (SPEC §9.6). See SmokeM10.h.

#include "SmokeM10.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "CliCheck.h"
#include "engine/NodeMeters.h"
#include "engine/NodeOverrides.h"
#include "patch/PatchBake.h"
#include "patch/PatchPlayer.h"
#include "patch/PatchTweaks.h"
#include "patch/PatchValidator.h"

namespace vibesynth_cli {

namespace {

using namespace vibesynth;

constexpr double kRate = 48000.0;
constexpr double kVoiceRate = 96000.0;

struct Ev
{
    int samplePosition;
    const uint8_t* data;
    int numBytes;
};

bool sameBits (const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0);
}

double maxDiffDb (const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size())
        return 0.0;
    double d = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        d = std::fmax (d, std::fabs ((double) a[i] - (double) b[i]));
    return d > 0.0 ? 20.0 * std::log10 (d) : -400.0;
}

// One voice VM on its own (no player): voice code that makes its own test
// input, rendered with or without an override table.
std::vector<float> runVoice (const std::string& code, const OverrideTable* table, int samples = 4000,
                             double m2 = 0.37)
{
    const PatchStrings ps { "", code.c_str(), "" };
    EelVoice v (ps, kVoiceRate, 7u);
    std::vector<float> out;
    if (! v.isValid())
        return out;
    v.rack().overrides = table;
    v.rackContext().hooked = table != nullptr;
    double macros[8] = { 0.5, m2, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5 };
    v.noteOn (110.0, 45.0, 0.8);
    for (int i = 0; i < samples; ++i)
        out.push_back (v.renderSample (110.0, i < samples / 2 ? 1.0 : 0.0, 45.0, 0.8, macros));
    return out;
}

// The master VM on its own: L/R test input in, L and R out (interleaved).
std::vector<float> runMaster (const std::string& code, const OverrideTable* table, int samples = 4000)
{
    const PatchStrings ps { "", "out = 0;", code.c_str() };
    EelMaster m (ps, kRate);
    std::vector<float> out;
    if (! m.isValid())
        return out;
    m.voiceRack().overrides = table;
    m.rackContext().hooked = table != nullptr;
    double macros[8] = { 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5 };
    for (int i = 0; i < samples; ++i)
    {
        const float inL = (float) (0.6 * std::sin (i * 0.031));
        const float inR = (float) (0.5 * std::sin (i * 0.047 + 1.0));
        float l = 0.0f, r = 0.0f;
        m.renderSample (inL, inR, l, r, macros);
        out.push_back (l - inL);
        out.push_back (r - inR);
    }
    return out;
}

bool allZero (const std::vector<float>& v)
{
    if (v.empty())
        return false;
    for (const float x : v)
        if (x != 0.0f)
            return false;
    return true;
}

// The phrase every render test plays: three notes, a chord, all macros
// moving (so macro maps matter), 140 bpm.
std::vector<float> renderPhrase (const Patch& patch, const OverrideTable* table, TraceBank* traces,
                                 double seconds = 2.4)
{
    std::string error;
    auto program = PatchProgram::build (patch, kRate, error);
    std::vector<float> out;
    if (program == nullptr)
        return out;
    float targets[PatchPlayer::kNumMacros];
    for (int m = 0; m < PatchPlayer::kNumMacros; ++m)
        targets[m] = (float) patch.macroDefault (m);
    PatchPlayer player;
    player.setProgram (program.get());
    player.prepare (kRate, 256, targets);
    player.setHostTransport (140.0, false, false, 0.0);
    player.setVoiceControls (PatchPlayer::VoiceMode::Patch, 0.2);
    player.setNodeHooks (table, traces);

    static const uint8_t on40[3] = { 0x90, 40, 100 }, off40[3] = { 0x80, 40, 0 };
    static const uint8_t on45[3] = { 0x90, 45, 90 }, off45[3] = { 0x80, 45, 0 };
    static const uint8_t on52[3] = { 0x90, 52, 90 }, off52[3] = { 0x80, 52, 0 };
    static const uint8_t on28[3] = { 0x90, 28, 110 }, off28[3] = { 0x80, 28, 0 };
    auto at = [] (double s) { return (int) std::lround (s * kRate); };
    const std::vector<Ev> events = { { at (0.0), on40, 3 },  { at (0.5), off40, 3 }, { at (0.6), on45, 3 },
                                     { at (0.6), on52, 3 },  { at (1.3), off45, 3 }, { at (1.3), off52, 3 },
                                     { at (1.4), on28, 3 },  { at (2.0), off28, 3 } };
    auto it = events.cbegin();
    const int total = at (seconds);
    for (int start = 0; start < total; start += 256)
    {
        const double t = start / kRate;
        for (int m = 0; m < PatchPlayer::kNumMacros; ++m)
            targets[m] = (float) (0.5 + 0.45 * std::sin (6.2831853 * t * (0.3 + 0.17 * m) + m));
        const int n = std::min (256, total - start);
        player.renderChunk (n, targets, it, events.cend(), start);
        for (int i = 0; i < n; ++i)
        {
            out.push_back (player.outL()[i]);
            out.push_back (player.outR()[i]);
        }
    }
    return out;
}

std::vector<NodeRef> nodeList (const TouchedNodes& t)
{
    std::vector<NodeRef> nodes;
    for (int rk = 0; rk < 2; ++rk)
        for (int m = 0; m < kNumModules; ++m)
            for (int s = 0; s < kMaxSlotsPerModule; ++s)
                if (((rk == 0 ? t.voice[m] : t.master[m]) >> s) & 1u)
                    nodes.push_back ({ (RackKind) rk, (ModuleId) m, s });
    return nodes;
}

//==============================================================================
bool smokeMetadata()
{
    bool ok = true;
    int primaries[kNumModules] = {};
    int bad = 0;
    std::string first;
    for (int i = 0; i < numLexiconEntries(); ++i)
    {
        const LexiconEntry& e = lexiconEntry (i);
        auto fail = [&] (const std::string& why)
        {
            if (first.empty())
                first = std::string (e.name) + ": " + why;
            ++bad;
        };
        if (e.numArgs > kMaxLexiconArgs)
            fail ("too many arguments");
        // The argument names spell the documented signature.
        std::string sig;
        for (int k = 0; k < e.numArgs && k < kMaxLexiconArgs; ++k)
            sig += (k > 0 ? ", " : "") + std::string (e.args[k].name);
        if (sig != e.params)
            fail ("argument names \"" + sig + "\" != params \"" + e.params + "\"");
        if (e.numSlots > 0 && (e.numArgs < 1 || e.args[0].kind != ArgKind::Slot))
            fail ("slot argument missing");
        for (int k = 0; k < e.numArgs && k < kMaxLexiconArgs; ++k)
            if (isEditableArg (e.args[k].kind) && ! (e.args[k].lo < e.args[k].hi))
                fail ("empty range");
        if (e.bypass == BypassKind::PassArg
            && (e.bypassArg < 0 || e.bypassArg >= e.numArgs || e.args[e.bypassArg].kind != ArgKind::Audio))
            fail ("bypass passes a non-signal argument");
        if (e.bypass == BypassKind::ForceArg
            && (e.bypassArg < 0 || e.bypassArg >= e.numArgs || ! isEditableArg (e.args[e.bypassArg].kind)))
            fail ("bypass forces a bad argument");
        if (e.trace == TraceKind::ByRate && (e.traceArg < 1 || e.traceArg >= e.numArgs))
            fail ("trace rate argument out of range");
        if (e.module >= kNumModules)
            fail ("module out of range");
        else if (e.module >= 0)
        {
            primaries[e.module] += e.primary ? 1 : 0;
            if (std::string (e.name).rfind (moduleShortName ((ModuleId) e.module), 0) != 0)
                fail ("module name mismatch");
        }
    }
    for (int m = 0; m < kNumModules; ++m)
        if (primaries[m] != 1)
        {
            ++bad;
            if (first.empty())
                first = std::string (moduleShortName ((ModuleId) m)) + " has " + std::to_string (primaries[m])
                        + " primary functions";
        }
    ok = check (bad == 0, "M10 lexicon metadata: names match params, one primary per module, ranges and bypass "
                          "arguments valid%s%s", bad ? "; first problem: " : "", first.c_str());

    // Node ids round-trip for every module, slot and region.
    int idBad = 0;
    for (int rk = 0; rk < 2; ++rk)
        for (int m = 0; m < kNumModules; ++m)
            for (int s = 0; s < kMaxSlotsPerModule; ++s)
            {
                const ModuleId mod = (ModuleId) m;
                const bool slotless = mod == ModuleId::BusMonoLow || mod == ModuleId::BusChorus
                                   || mod == ModuleId::BusReverb;
                if (slotless && s > 0)
                    continue;
                const NodeRef n { (RackKind) rk, mod, s };
                NodeRef back;
                if (! parseNodeId (nodeIdString (n), back) || ! (back == n))
                    ++idBad;
            }
    NodeRef a, b, c;
    const bool spelled = nodeIdString ({ RackKind::Voice, ModuleId::FltLp, 0 }) == "flt_lp:0"
                      && nodeIdString ({ RackKind::Master, ModuleId::BusSat, 1 }) == "master/bus_sat#1"
                      && nodeIdString ({ RackKind::Voice, ModuleId::FxSat, 2 }) == "fx_sat#2"
                      && nodeIdString ({ RackKind::Master, ModuleId::BusReverb, 0 }) == "master/bus_reverb"
                      && parseNodeId ("flt_lp", a) && a.slot == 0 && parseNodeId ("util_xover_hi:1", b)
                      && b.module == ModuleId::UtilXover && b.slot == 1 && ! parseNodeId ("noise", c)
                      && ! parseNodeId ("flt_lp:9", c) && ! parseNodeId ("flt_lp:x", c);
    ok = check (idBad == 0 && spelled, "M10 node ids: \"flt_lp:0\", \"fx_sat#2\", \"master/bus_sat#1\", "
                                       "\"master/bus_reverb\"; all %d round-trip%s",
                2 * kNumModules * kMaxSlotsPerModule, idBad ? " (some don't)" : "")
       && ok;
    return ok;
}

bool smokeOverrideMath()
{
    bool ok = true;
    const LexiconArg norm { "cutoff", "Cutoff", ArgKind::Norm, 0.0, 1.0 };
    const LexiconArg hz { "hz", "Pitch", ArgKind::Hz, 20.0, 20000.0 };
    const LexiconArg axis { "axis", "Axis", ArgKind::Choice, 0.0, 2.0 };
    auto off = [] (const LexiconArg& a, double user, double x)
    {
        return overrideArg (a, ArgMode::Offset, offsetApplied (a, user), 0.0, 0.0, 0.0, x);
    };
    ok = check (off (norm, 0.25, 0.1) == 0.35 && off (norm, 0.25, 0.9) == 1.0 && off (norm, -0.5, 0.2) == 0.0
                    && off (norm, 0.0, 0.42) == 0.42,
                "M10 offset (normalized): 0.1 + 0.25 = 0.35, clamps to 1 and to 0")
       && ok;
    ok = check (offsetApplied (hz, 12.0) == 2.0 && off (hz, 12.0, 220.0) == 440.0 && off (hz, -12.0, 220.0) == 110.0
                    && std::fabs (off (hz, 7.0, 440.0) - 659.2551138257398) < 1.0e-9 && off (hz, 0.0, 333.0) == 333.0,
                "M10 offset (hz) in semitones: +12 doubles, -12 halves, +7 is a fifth")
       && ok;
    ok = check (off (axis, 0.5, 0.0) == 1.0 && off (axis, 0.75, 1.0) == 2.0,
                "M10 offset (choice) works in the range (axis 0-2) and clamps")
       && ok;
    const double rep = replaceApplied (norm, 1.7);
    ok = check (rep == 1.0 && overrideArg (norm, ArgMode::Replace, rep, 0, 0, 0, 0.123) == 1.0
                    && replaceApplied (hz, 5.0) == 20.0 && replaceApplied (norm, 0.3) == 0.3,
                "M10 replace ignores the code's value and clamps to the range")
       && ok;
    const double mapped = overrideArg (norm, ArgMode::Macro, 0.0, 0.2, 0.8, 0.5, 0.9);
    const double inverse = overrideArg (norm, ArgMode::Macro, 0.0, 0.8, 0.2, 1.0, 0.9);
    ok = check (std::fabs (mapped - 0.5) < 1.0e-12 && std::fabs (inverse - 0.2) < 1.0e-12
                    && overrideArg (norm, ArgMode::None, 0.0, 0, 0, 0, 0.42) == 0.42,
                "M10 macro map lo + m * (hi - lo) (0.2..0.8 at m 0.5 = 0.5; an inverted map works)")
       && ok;

    // In the engine: an override equals the same value written in code.
    OverrideTable t;
    const std::string in = "x = sin(ph); ph += 0.05; ";
    const int lp = lexiconArgIndex (*findLexiconEntry ("flt_lp"), "cutoff");
    t.setArg (RackKind::Voice, ModuleId::FltLp, 0, lp, ArgMode::Replace, 0.62);
    const auto replaced = runVoice (in + "out = flt_lp(0, x, 0.3, 0.4);", &t);
    const auto literal = runVoice (in + "out = flt_lp(0, x, 0.62, 0.4);", nullptr);
    ok = check (! replaced.empty() && sameBits (replaced, literal),
                "M10 engine: replace cutoff 0.62 == the code with 0.62 (bit-identical)")
       && ok;

    t.clear();
    t.setArg (RackKind::Voice, ModuleId::OscSaw, 0, 1, ArgMode::Offset, 12.0);
    ok = check (sameBits (runVoice ("out = osc_saw(0, 220);", &t), runVoice ("out = osc_saw(0, 440);", nullptr)),
                "M10 engine: osc_saw pitch +12 st == osc_saw at twice the Hz")
       && ok;

    t.clear();
    t.setArg (RackKind::Voice, ModuleId::FltLp, 0, lp, ArgMode::Macro, 0.0, 1, 0.1, 0.9);
    const std::string mapCode = in + "out = flt_lp(0, x, 0.1 + m2 * " + formatEelNumber (0.9 - 0.1) + ", 0.4);";
    ok = check (sameBits (runVoice (in + "out = flt_lp(0, x, 0.3, 0.4);", &t), runVoice (mapCode, nullptr)),
                "M10 engine: cutoff mapped to m2 (0.1..0.9) == the code 0.1 + m2 * 0.8")
       && ok;

    // Only the tweaked slot changes; clearing restores the original.
    t.clear();
    t.setArg (RackKind::Voice, ModuleId::FltLp, 1, lp, ArgMode::Replace, 0.9);
    ok = check (sameBits (runVoice (in + "out = flt_lp(0, x, 0.3, 0.4);", &t),
                          runVoice (in + "out = flt_lp(0, x, 0.3, 0.4);", nullptr))
                    && t.activeNode (0, (int) ModuleId::FltLp, 1) != nullptr && t.anyFor (RackKind::Voice),
                "M10 engine: a tweak on flt_lp:1 leaves flt_lp:0 alone")
       && ok;
    t.clearArg (RackKind::Voice, ModuleId::FltLp, 1, lp);
    ok = check (t.activeNode (0, (int) ModuleId::FltLp, 1) == nullptr && ! t.anyFor (RackKind::Voice),
                "M10 clearing the last override deactivates the node and the rack")
       && ok;
    return ok;
}

bool smokeBypass()
{
    bool ok = true;
    const std::string in = "x = sin(ph); ph += 0.05; ";
    OverrideTable t;

    t.setBypass (RackKind::Voice, ModuleId::FltLp, 0, true);
    ok = check (allZero (runVoice (in + "out = flt_lp(0, x, 0.3, 0.4) - x;", &t)),
                "M10 bypass (filter): flt_lp passes its input through exactly")
       && ok;
    t.clear();
    t.setBypass (RackKind::Voice, ModuleId::OscSaw, 0, true);
    ok = check (allZero (runVoice ("out = osc_saw(0, 220);", &t)), "M10 bypass (oscillator): osc_saw outputs 0") && ok;
    t.clear();
    t.setBypass (RackKind::Voice, ModuleId::UtilSafeFb, 0, true);
    ok = check (allZero (runVoice (in + "out = util_safe_fb(0, x);", &t)),
                "M10 bypass (util_safe_fb): outputs 0, feedback off")
       && ok;
    t.clear();
    t.setBypass (RackKind::Voice, ModuleId::EnvAdsr, 0, true);
    const std::string env = "out = env_adsr(0, gate, 0.1, 0.2, 0.5, 0.3);";
    ok = check (sameBits (runVoice (env, &t), runVoice (env, nullptr)) && ! allZero (runVoice (env, nullptr)),
                "M10 bypass (envelope): env_adsr has none (output unchanged)")
       && ok;
    t.clear();
    t.setBypass (RackKind::Voice, ModuleId::UtilXover, 0, true);
    const auto xo = runVoice (in + "lo = util_xover_lo(0, x); hi = util_xover_hi(0); out = lo - x + hi;", &t);
    ok = check (allZero (xo), "M10 bypass (crossover): util_xover_lo passes, util_xover_hi gives 0") && ok;

    // Stateless shapers are numbered by call order: #1 is the second call.
    t.clear();
    t.setBypass (RackKind::Voice, ModuleId::FxSat, 1, true);
    ok = check (allZero (runVoice (in + "a = fx_sat(x, 0.8); b = fx_sat(x, 0.8); out = b - x;", &t))
                    && ! allZero (runVoice (in + "a = fx_sat(x, 0.8); b = fx_sat(x, 0.8); out = a - x;", &t)),
                "M10 bypass (stateless): fx_sat#1 passes through, fx_sat#0 still saturates")
       && ok;
    t.clear();
    t.setArg (RackKind::Voice, ModuleId::FxSat, 0, 1, ArgMode::Replace, 0.5);
    ok = check (allZero (runVoice (in + "out = fx_sat(x, 0.2) - fx_sat(x, 0.5);", &t)),
                "M10 stateless override: drive on fx_sat#0 only")
       && ok;
    // Nested calls number inner first (the order calls complete).
    t.clear();
    t.setBypass (RackKind::Voice, ModuleId::FxSat, 0, true);
    ok = check (sameBits (runVoice (in + "out = fx_sat(fx_sat(x, 0.9), 0.4);", &t),
                          runVoice (in + "out = fx_sat(x, 0.4);", nullptr)),
                "M10 stateless numbering: in fx_sat(fx_sat(x)), the inner call is #0")
       && ok;

    // Master: bus_reverb runs dry (mix 0) for both channels; bus_sat#1 is R.
    t.clear();
    t.setBypass (RackKind::Master, ModuleId::BusReverb, 0, true);
    ok = check (allZero (runMaster ("outl = bus_reverb(inl, inr, 0.7, 0.6); outr = bus_reverb_r();", &t)),
                "M10 bypass (bus_reverb): mix 0, both channels dry")
       && ok;
    t.clear();
    t.setBypass (RackKind::Master, ModuleId::BusSat, 1, true);
    const auto sat = runMaster ("outl = bus_sat(inl, 0.6); outr = bus_sat(inr, 0.6);", &t);
    bool rDry = ! sat.empty(), lWet = false;
    for (size_t i = 0; i + 1 < sat.size(); i += 2)
    {
        rDry = rDry && sat[i + 1] == 0.0f;
        lWet = lWet || sat[i] != 0.0f;
    }
    ok = check (rDry && lWet, "M10 bypass (master/bus_sat#1): the second call (R) passes, L still saturates") && ok;
    // A voice-rack override doesn't reach the master's nodes.
    t.clear();
    t.setBypass (RackKind::Voice, ModuleId::BusSat, 0, true);
    ok = check (sameBits (runMaster ("outl = bus_sat(inl, 0.6); outr = bus_sat(inr, 0.6);", &t),
                          runMaster ("outl = bus_sat(inl, 0.6); outr = bus_sat(inr, 0.6);", nullptr)),
                "M10 voice and master nodes are separate (bus_sat#0 vs master/bus_sat#0)")
       && ok;
    return ok;
}

bool smokePersistence()
{
    bool ok = true;
    Patch p = parsePatch (builtinPresetText (0)).patch;
    const std::string plain = serializePatch (p);
    ok = check (p.tweaks.empty() && p.macroMaps.empty() && plain.find ("tweaks") == std::string::npos
                    && plain.find ("macroMaps") == std::string::npos,
                "M10 an untweaked patch has no tweaks/macroMaps keys (older files unchanged)")
       && ok;

    setPatchArgTweak (p, "flt_lp:0", "cutoff", ArgTweak::kOffset, 0.123456789);
    setPatchArgTweak (p, "flt_lp:0", "res", ArgTweak::kReplace, 0.4);
    setPatchBypass (p, "fx_fold#0", true);
    setPatchArgTweak (p, "osc_supersaw:0", "hz", ArgTweak::kOffset, -7.0);
    setPatchMacroMap (p, "master/bus_sat#0", "drive", 6, 0.1, 0.65);
    setPatchMacroMap (p, "osc_supersaw:0", "detune", 7, 0.9, 0.2);
    const std::string text = serializePatch (p);
    const auto back = parsePatch (text);
    ok = check (back.ok() && back.patch == p && back.patch.tweaks.size() == 3 && back.patch.macroMaps.size() == 2
                    && serializePatch (back.patch) == text && back.patch.tweaks[0].args[0].value == 0.123457,
                "M10 tweaks and macroMaps round-trip through .pulsar exactly (values at 1e-6)")
       && ok;

    // The plugin state: the patch JSON as a property of the APVTS tree.
    juce::ValueTree state ("PARAMETERS");
    state.setProperty ("patchJson", juce::String (text), nullptr);
    const auto xml = state.createXml();
    const auto again = juce::parseXML (xml->toString());
    const auto tree = again != nullptr ? juce::ValueTree::fromXml (*again) : juce::ValueTree();
    const auto restored = parsePatch (tree.getProperty ("patchJson").toString().toStdString());
    ok = check (restored.ok() && restored.patch == p, "M10 tweaks round-trip through the plugin state (XML)") && ok;

    // Exclusive per argument, and canonical lists.
    Patch q = p;
    setPatchMacroMap (q, "flt_lp:0", "cutoff", 2, 0.0, 1.0);
    const bool mapWins = findArgTweak (q, "flt_lp:0", "cutoff") == nullptr && findMacroMap (q, "flt_lp:0", "cutoff");
    setPatchArgTweak (q, "flt_lp:0", "cutoff", ArgTweak::kReplace, 0.5);
    const bool tweakWins = findMacroMap (q, "flt_lp:0", "cutoff") == nullptr && findArgTweak (q, "flt_lp:0", "cutoff");
    setPatchBypass (q, "fx_fold#0", false);
    ok = check (mapWins && tweakWins && findNodeTweak (q, "fx_fold#0") == nullptr,
                "M10 an argument is tweaked or mapped, never both; an emptied node is dropped")
       && ok;

    // Malformed entries are skipped, never an error.
    const std::string bad = R"({"version": 1, "code": {"voice": "out = 0;"},
        "tweaks": [5, {"node": 3}, {"node": "flt_lp:0", "args": {"cutoff": {"mode": "bogus", "value": 1}}},
                   {"node": "flt_lp:0", "args": {"res": {"mode": "replace", "value": "x"}}}],
        "macroMaps": [{"macro": 9, "node": "flt_lp:0", "arg": "cutoff"}, {"macro": 2}]})";
    const auto skipped = parsePatch (bad);
    ok = check (skipped.ok() && skipped.patch.macroMaps.empty() && skipped.patch.tweaks.size() == 1
                    && skipped.patch.tweaks[0].args.size() == 1 && skipped.patch.tweaks[0].args[0].value == 0.0,
                "M10 malformed tweaks are skipped (the patch still loads)")
       && ok;

    // Applying: names resolve, a new patch clears the table.
    OverrideTable table;
    const int applied = applyPatchTweaks (p, table);
    const int cutoff = lexiconArgIndex (*findLexiconEntry ("flt_lp"), "cutoff");
    const auto* lp = table.activeNode (0, (int) ModuleId::FltLp, 0);
    const auto* bus = table.activeNode (1, (int) ModuleId::BusSat, 0);
    ok = check (applied == 6 && lp != nullptr && bus != nullptr
                    && lp->args[cutoff].mode.load() == (uint8_t) ArgMode::Offset
                    && bus->args[1].mode.load() == (uint8_t) ArgMode::Macro && bus->args[1].macro.load() == 5,
                "M10 applying a patch's tweaks fills the table (%d of 6 entries)", applied)
       && ok;
    applyPatchTweaks (parsePatch (builtinPresetText (1)).patch, table);
    ok = check (! table.anyFor (RackKind::Voice) && ! table.anyFor (RackKind::Master),
                "M10 a new patch without tweaks clears them")
       && ok;
    return ok;
}

bool smokeTraces()
{
    const Patch reese = parsePatch (builtinPresetText (0)).patch;
    auto bank = std::make_unique<TraceBank>();
    bank->enabled = true;
    const auto out = renderPhrase (reese, nullptr, bank.get(), 1.0);
    std::vector<float> buf (NodeTrace::kPoints);
    int n = 0;
    bool ring = false;
    const bool lpOk = readNodeTrace (bank->voice.nodes[(int) ModuleId::FltLp][0], buf.data(), n, ring) && ! ring
                   && n >= 16;
    float lo = 1.0f, hi = -1.0f;
    for (int i = 0; i < n; ++i)
    {
        lo = std::fmin (lo, buf[(size_t) i]);
        hi = std::fmax (hi, buf[(size_t) i]);
    }
    int envN = 0;
    bool envRing = false;
    const bool envOk = readNodeTrace (bank->voice.nodes[(int) ModuleId::EnvAdsr][0], buf.data(), envN, envRing)
                    && envRing && envN > 64;
    const float cutoff = bank->voice.nodes[(int) ModuleId::FltLp][0].args[2].load();
    const bool masterOk = readNodeTrace (bank->master.nodes[(int) ModuleId::BusSat][0], buf.data(), n, ring);
    bool ok = check (lpOk && hi > lo, "M10 trace: flt_lp:0 publishes a triggered cycle (%d points, %.2f..%.2f)", n, lo,
                     hi);
    ok = check (envOk, "M10 trace: env_adsr:0 rolls a control trace (%d points over ~1 s)", envN) && ok;
    ok = check (cutoff >= 0.0f && cutoff <= 1.0f && masterOk,
                "M10 trace: live argument values (flt_lp:0 cutoff %.3f) and master nodes", cutoff)
       && ok;
    // Traces are written only while enabled.
    auto off = std::make_unique<TraceBank>();
    renderPhrase (reese, nullptr, off.get(), 0.3);
    ok = check (off->voice.nodes[(int) ModuleId::FltLp][0].frameSeq.load() == 0u,
                "M10 trace: nothing is captured while traces are disabled")
       && ok;
    return ok;
}

bool smokeBakeRules()
{
    bool ok = true;
    const std::string e6 = formatEelNumber (1.0e-6);
    ok = check (formatEelNumber (0.5) == "0.5" && formatEelNumber (-0.25) == "(-0.25)"
                    && e6.find ('e') == std::string::npos && std::strtod (e6.c_str(), nullptr) == 1.0e-6
                    && formatEelNumber (std::exp2 (7.0 / 12.0)).size() > 10,
                "M10 bake numbers: shortest exact, no exponent (1e-6 -> %s), negatives in parentheses", e6.c_str())
       && ok;

    Patch p;
    p.voice = "x = sin(ph); ph += 0.05;\nout = trig ? fx_sat(x, 0.5) : fx_sat(x, 0.2);";
    setPatchArgTweak (p, "fx_sat#0", "drive", ArgTweak::kReplace, 0.7);
    const auto refused = bakePatch (p);
    ok = check (! refused.ok && refused.error.find ("branch") != std::string::npos,
                "M10 bake refuses a tweaked shaper whose call order isn't fixed: \"%s\"", refused.error.c_str())
       && ok;
    p.tweaks.clear();
    setPatchArgTweak (p, "osc_saw:0", "hz", ArgTweak::kOffset, 3.0);
    ok = check (bakePatch (p).ok, "M10 bake accepts the same patch when its branch shapers are untweaked") && ok;

    Patch q;
    q.voice = "s = 1; out = osc_saw(s, 220);";
    setPatchArgTweak (q, "osc_saw:1", "hz", ArgTweak::kOffset, 3.0);
    const auto slot = bakePatch (q);
    ok = check (! slot.ok && slot.error.find ("slot") != std::string::npos,
                "M10 bake refuses a tweaked call whose slot isn't a literal")
       && ok;

    Patch r;
    r.voice = "// osc_saw(0, 1) in a comment\nx = osc_saw(0, 110 /* hz */) * 0.5;\nout = flt_lp(1, x, 0.3, 0.2);";
    setPatchArgTweak (r, "osc_saw:0", "hz", ArgTweak::kOffset, 12.0);
    setPatchArgTweak (r, "flt_lp:1", "cutoff", ArgTweak::kOffset, -0.1);
    setPatchArgTweak (r, "flt_lp:2", "cutoff", ArgTweak::kOffset, 0.1);
    setPatchMacroMap (r, "flt_lp:1", "res", 3, 0.0, 0.8);
    const auto baked = bakePatch (r);
    ok = check (baked.ok && baked.callsChanged == 2 && baked.patch.voice.find ("// osc_saw(0, 1) in a comment") == 0
                    && baked.patch.voice.find ("* 2)") != std::string::npos
                    && baked.patch.voice.find ("min(max((") != std::string::npos
                    && baked.patch.voice.find ("m3 * 0.8") != std::string::npos && baked.patch.tweaks.empty()
                    && baked.patch.macroMaps.empty() && baked.notes.size() == 2
                    && baked.patch.macros.size() == 3 && baked.patch.macros[2].label == "Resonance",
                "M10 bake rewrites call sites (comments untouched), drops tweaks with no call, labels unused m3")
       && ok;
    if (! baked.ok || baked.callsChanged != 2)
        std::printf ("       baked voice: %s\n       error: %s\n", baked.patch.voice.c_str(), baked.error.c_str());
    return ok;
}

// Random tweaks for every built-in, then the tweaked render must equal the
// baked patch's render.
void randomTweaks (Patch& p, const std::vector<NodeRef>& nodes, std::mt19937& rng, bool stateless)
{
    std::uniform_real_distribution<double> u (0.0, 1.0);
    for (const auto& n : nodes)
    {
        const LexiconEntry* e = primaryLexiconEntry ((int) n.module);
        if (e == nullptr || (! stateless && isStatelessModule (n.module)) || u (rng) < 0.4)
            continue;
        const std::string id = nodeIdString (n);
        if ((e->bypass == BypassKind::PassArg || e->bypass == BypassKind::ForceArg) && u (rng) < 0.2)
            setPatchBypass (p, id, true);
        for (int k = 0; k < e->numArgs; ++k)
        {
            const LexiconArg& a = e->args[k];
            if (! isEditableArg (a.kind))
                continue;
            const double r = u (rng);
            const bool hz = a.kind == ArgKind::Hz;
            if (r < 0.3)
                setPatchArgTweak (p, id, a.name, ArgTweak::kOffset, hz ? (u (rng) * 14.0 - 7.0) : (u (rng) * 0.6 - 0.3));
            else if (r < 0.5)
            {
                double v = a.lo + u (rng) * (a.hi - a.lo);
                if (hz)
                    v = std::fmin (a.hi, std::fmax (a.lo, 60.0 * std::pow (20.0, u (rng))));
                if (a.kind == ArgKind::Choice)
                    v = std::floor (v + 0.5);
                setPatchArgTweak (p, id, a.name, ArgTweak::kReplace, v);
            }
            else if (r < 0.65)
            {
                const double lo = hz ? 80.0 + u (rng) * 200.0 : u (rng) * 0.5;
                const double hi = hz ? 300.0 + u (rng) * 900.0 : 0.5 + u (rng) * 0.5;
                setPatchMacroMap (p, id, a.name, 1 + (int) (u (rng) * 7.999), lo, hi);
            }
        }
    }
}

bool smokeBakeEquivalence()
{
    bool ok = true;
    int passed = 0, refusals = 0, rerolls = 0, totalTweaks = 0;
    double worst = -400.0;
    for (int i = 0; i < numBuiltinPresets(); ++i)
    {
        const Patch base = parsePatch (builtinPresetText (i)).patch;
        const auto nodes = nodeList (probeTouchedNodes (base, kRate));
        bool done = false;
        std::string why;
        for (int attempt = 0; attempt < 8 && ! done; ++attempt)
        {
            std::mt19937 rng (0xB4CE0000u + (uint32_t) (i * 97 + attempt));
            Patch tweaked = base;
            randomTweaks (tweaked, nodes, rng, attempt < 4);
            if (tweaked.tweaks.empty() && tweaked.macroMaps.empty())
                continue;
            const auto baked = bakePatch (tweaked);
            if (! baked.ok)
            {
                ++refusals;
                why = baked.error;
                continue;
            }
            const auto vr = validatePatch (baked.patch, kRate);
            if (! vr.ok())
            {
                // The tweaks themselves made it silent/loud: try other ones.
                ++rerolls;
                why = vr.error;
                continue;
            }
            auto table = std::make_unique<OverrideTable>();
            applyPatchTweaks (tweaked, *table);
            Patch bakedSameTrim = baked.patch;
            bakedSameTrim.trim = tweaked.trim;
            const auto a = renderPhrase (tweaked, table.get(), nullptr);
            const auto b = renderPhrase (bakedSameTrim, nullptr, nullptr);
            const double db = maxDiffDb (a, b);
            worst = std::fmax (worst, db);
            const int count = (int) (tweaked.macroMaps.size());
            int args = count;
            for (const auto& t : tweaked.tweaks)
                args += (int) t.args.size() + (t.bypass ? 1 : 0);
            totalTweaks += args;
            ok = check (! a.empty() && a.size() == b.size() && db <= -100.0,
                        "M10 bake == tweaked render: %-22s %2d tweaks, %d calls rewritten, max diff %s",
                        builtinPresetFile (i), args, baked.callsChanged,
                        db <= -399.0 ? "0 (bit-identical)" : (std::to_string (db) + " dB").c_str())
               && ok;
            passed += db <= -100.0 ? 1 : 0;
            done = true;
        }
        if (! done)
            ok = check (false, "M10 bake equivalence: %s found no tweak set that bakes and validates (%s)",
                        builtinPresetFile (i), why.c_str())
               && ok;
    }
    std::printf ("[INFO] M10 bake equivalence: %d of %d built-ins, %d tweaks in all, worst diff %s; %d refused "
                 "(shaper order), %d re-rolled (tweaks failed the offline test)\n",
                 passed, numBuiltinPresets(), totalTweaks,
                 worst <= -399.0 ? "0" : (std::to_string (worst) + " dB").c_str(), refusals, rerolls);
    return ok;
}

bool smokeNoOverrideIdentical()
{

    bool ok = true;
    int same = 0;
    for (int i = 0; i < numBuiltinPresets(); ++i)
    {
        const Patch p = parsePatch (builtinPresetText (i)).patch;
        auto table = std::make_unique<OverrideTable>();
        auto bank = std::make_unique<TraceBank>();
        bank->enabled = true;
        const auto plain = renderPhrase (p, nullptr, nullptr);
        const auto hooked = renderPhrase (p, table.get(), bank.get());
        // A tweak set and then removed leaves nothing behind.
        table->setArg (RackKind::Voice, ModuleId::FltLp, 0, 2, ArgMode::Replace, 0.9);
        table->clearArg (RackKind::Voice, ModuleId::FltLp, 0, 2);
        const auto cleared = renderPhrase (p, table.get(), nullptr);
        const bool identical = ! plain.empty() && sameBits (plain, hooked) && sameBits (plain, cleared);
        same += identical ? 1 : 0;
        if (! identical)
        {
            size_t first = 0;
            while (first < plain.size() && first < hooked.size() && plain[first] == hooked[first])
                ++first;
            ok = check (false, "M10 no-override render of %s is bit-identical (traces on: %s, first diff at %d; "
                               "cleared table: %s)",
                        builtinPresetFile (i), sameBits (plain, hooked) ? "same" : "differs", (int) first / 2,
                        sameBits (plain, cleared) ? "same" : "differs")
               && ok;
        }
    }
    ok = check (same == numBuiltinPresets(),
                "M10 no-override renders bit-identical with the hooks attached (empty table + traces on): %d of %d",
                same, numBuiltinPresets())
       && ok;

    // What the trace voice costs (report only).
    const Patch reese = parsePatch (builtinPresetText (0)).patch;
    auto bank = std::make_unique<TraceBank>();
    bank->enabled = true;
    auto time = [&] (TraceBank* b)
    {
        renderPhrase (reese, nullptr, b, 0.5);
        const auto t0 = std::chrono::steady_clock::now();
        renderPhrase (reese, nullptr, b, 2.4);
        return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    };
    const double plainMs = time (nullptr);
    const double tracedMs = time (bank.get());
    auto other = std::make_unique<OverrideTable>();
    other->setBypass (RackKind::Voice, ModuleId::FltBp, 2, true);  // a node Reese doesn't have
    renderPhrase (reese, other.get(), nullptr, 0.5);
    const auto t0 = std::chrono::steady_clock::now();
    renderPhrase (reese, other.get(), nullptr, 2.4);
    const double hookedMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    std::printf ("[INFO] M10 Reese (mono) phrase render: %.1f ms plain, %.1f ms with a tweak elsewhere in the voice "
                 "(%+.0f%%), %.1f ms with traces on (%+.0f%%; one voice and the master, only while an editor is open)\n",
                 plainMs, hookedMs, 100.0 * (hookedMs / plainMs - 1.0), tracedMs, 100.0 * (tracedMs / plainMs - 1.0));
    return ok;
}

} // namespace

bool smokeM10()
{
    bool ok = smokeMetadata();
    ok = smokeOverrideMath() && ok;
    ok = smokeBypass() && ok;
    ok = smokePersistence() && ok;
    ok = smokeTraces() && ok;
    ok = smokeBakeRules() && ok;
    ok = smokeBakeEquivalence() && ok;
    ok = smokeNoOverrideIdentical() && ok;
    return ok;
}

} // namespace vibesynth_cli
