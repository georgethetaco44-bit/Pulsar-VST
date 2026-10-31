#pragma once

// The .pulsar patch model (SPEC §9.4) plus parse/serialize.
//
// This header is JUCE-free so engine-side code (PatchProgram, the
// validator) can hold a Patch. Patch.cpp uses juce::JSON, so it is NOT in
// vibesynth_engine: JUCE modules are INTERFACE targets that compile their
// sources into every consumer, so the plugin and vibesynth_cli each
// compile Patch.cpp themselves (VIBESYNTH_PATCH_SOURCES in CMakeLists.txt).
//
// Numbers (macro defaults, trim) are quantised to 1e-6 on parse, so
// parse(serialize(p)) == p exactly.

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace vibesynth {

// Bitwise equality: "reads back identically" (no -Wfloat-equal, and NaN
// can't appear: parse() clamps every number).
inline bool samePatchNumber (double a, double b) noexcept
{
    return std::memcmp (&a, &b, sizeof (double)) == 0;
}

// Every way a patch can be refused, by parse() or by the validator
// (PatchValidator.h). The message that goes with it is user-readable.
enum class PatchStatus
{
    Ok,
    BadJson,       // not JSON, or the root is not an object
    BadField,      // version, voiceMode, macros or code have the wrong shape
    MissingVoice,  // code.voice absent or empty
    TooLarge,      // file > 256 KB or a code section > 32 KB (SPEC §7.1)
    LintError,     // unknown function, wrong argument count or slot (M8, before compiling)
    NestedLoops,   // loop/while nested more than 2 deep (checked before compiling)
    CompileError,  // EEL2 rejected a section
    NonFinite,     // NaN or Inf in the offline test
    TooLoud,       // peak above +12 dBFS before the limiter
    DcOffset,      // |mean| above 0.1 over the last 0.5 s
    Silent,        // all 9 configurations below -80 dBFS
    TooSlow        // CPU over 10 % of real time, or the 100 ms deadline
};

// Short stable name for logs and tests ("compile_error", "nan", ...).
const char* patchStatusName (PatchStatus s) noexcept;

struct MacroSpec
{
    std::string label;          // empty = the UI shows "Macro N"
    double defaultValue = 0.5;  // clamped to 0..1

    bool operator== (const MacroSpec& o) const
    {
        return label == o.label && samePatchNumber (defaultValue, o.defaultValue);
    }
};

// M10 (SPEC §9.6): the inspector's tweaks, saved as the optional "tweaks"
// and "macroMaps" keys. `node` is a node id (patch/PatchTweaks.h:
// "flt_lp:0", "fx_sat#1", "master/bus_reverb"), `arg` an argument name
// from the lexicon signature ("cutoff"). Values are quantised to 1e-6.
struct ArgTweak
{
    static constexpr int kOffset = 1;   // ArgMode::Offset
    static constexpr int kReplace = 2;  // ArgMode::Replace
    std::string arg;
    int mode = kOffset;
    double value = 0.0;   // offset: normalized amount (semitones for hz); replace: the value

    bool operator== (const ArgTweak& o) const
    {
        return arg == o.arg && mode == o.mode && samePatchNumber (value, o.value);
    }
};

struct NodeTweak
{
    std::string node;
    bool bypass = false;
    std::vector<ArgTweak> args;

    bool operator== (const NodeTweak& o) const { return node == o.node && bypass == o.bypass && args == o.args; }
};

struct MacroMap
{
    int macro = 1;        // 1..8 (m1..m8)
    std::string node;
    std::string arg;
    double min = 0.0;     // the argument's value at macro 0 ...
    double max = 1.0;     // ... and at macro 1

    bool operator== (const MacroMap& o) const
    {
        return macro == o.macro && node == o.node && arg == o.arg && samePatchNumber (min, o.min)
            && samePatchNumber (max, o.max);
    }
};

struct Patch
{
    static constexpr int kVersion = 1;
    static constexpr int kMaxMacros = 8;
    static constexpr size_t kMaxFileBytes = 256 * 1024;
    static constexpr size_t kMaxSectionBytes = 32 * 1024;

    int version = kVersion;
    std::string name;
    std::string prompt;
    std::vector<MacroSpec> macros;  // at most kMaxMacros
    std::string voiceMode = "poly"; // "poly" or "mono"; stored only until M7
    std::string graphJson;          // "graph" kept verbatim (compact JSON) for M9; empty = absent
    std::string init;
    std::string voice;
    std::string master;             // empty = inl/inr pass straight to outl/outr
    double trim = 1.0;              // output gain from the validator, 0 < trim <= 1
    std::vector<NodeTweak> tweaks;  // M10: optional; empty = absent from the file
    std::vector<MacroMap> macroMaps;

    // Design budget for the voice section per voice-sample (SPEC §4.2.1, M6):
    // a mono patch sounds one voice, so it gets 4x the poly figure.
    static constexpr double kPolyVoiceBudgetNs = 300.0;
    static constexpr double kMonoVoiceBudgetNs = 1200.0;
    static constexpr double kMasterBudgetNs = 2000.0;
    double voiceBudgetNs() const noexcept
    {
        return voiceMode == "mono" ? kMonoVoiceBudgetNs : kPolyVoiceBudgetNs;
    }

    // Macro i's default (0.5 when the patch lists fewer than i + 1 macros).
    double macroDefault (int i) const noexcept
    {
        return i >= 0 && i < static_cast<int> (macros.size()) ? macros[(size_t) i].defaultValue : 0.5;
    }

    bool operator== (const Patch& o) const
    {
        return version == o.version && name == o.name && prompt == o.prompt
            && macros == o.macros && voiceMode == o.voiceMode && graphJson == o.graphJson
            && init == o.init && voice == o.voice && master == o.master
            && samePatchNumber (trim, o.trim) && tweaks == o.tweaks && macroMaps == o.macroMaps;
    }
};

struct ParseResult
{
    PatchStatus status = PatchStatus::Ok;
    std::string error; // user-readable; empty when ok
    Patch patch;

    bool ok() const noexcept { return status == PatchStatus::Ok; }
};

// Parses .pulsar text. Validates version == 1, code.voice present and
// non-empty, voiceMode, macro count and the SPEC §7.1 size limits; clamps
// macro defaults to 0..1 and trim to (0, 1]; ignores unknown keys. M10:
// reads the optional "tweaks" and "macroMaps" (malformed entries are
// skipped, never an error, so a tweak can't make a patch unloadable).
ParseResult parsePatch (const std::string& text);

// Pretty JSON that parsePatch() reads back identically.
std::string serializePatch (const Patch& patch);

// "\r\n" (Windows) and a lone "\r" -> "\n". Every text parser runs this
// once where its input comes in (.pulsar files, model replies, the
// embedded prompt resources, the --batch prompt list), so a CRLF checkout
// or an editor's line endings never change what is parsed.
inline std::string normaliseLineEndings (std::string text)
{
    if (text.find ('\r') == std::string::npos)
        return text;
    std::string out;
    out.reserve (text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '\r')
            out += text[i];
        else if (i + 1 >= text.size() || text[i + 1] != '\n')
            out += '\n';
    }
    return out;
}

// Rounds to 1e-6 (the precision .pulsar numbers are stored with).
inline double quantisePatchNumber (double v) noexcept
{
    return std::round (v * 1.0e6) / 1.0e6;
}

// Built-in presets (resources/presets/*.pulsar, embedded with JUCE
// BinaryData). Index 0 is the default patch (Reese).
int numBuiltinPresets() noexcept;
const char* builtinPresetFile (int index) noexcept; // e.g. "reese.pulsar"
std::string builtinPresetText (int index);
std::string builtinPresetName (int index);          // the patch's "name"
// M9: built-ins [0, numShowcasePresets()) are the preset browser's
// Showcase section, the rest its Basic section (program order).
int numShowcasePresets() noexcept;

} // namespace vibesynth
