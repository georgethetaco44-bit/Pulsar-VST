// .pulsar parse/serialize (SPEC §9.4) and the embedded built-in presets.
// Compiled into the plugin and vibesynth_cli (not vibesynth_engine; see
// Patch.h). Never called on the audio thread.

#include "patch/Patch.h"

#include <juce_core/juce_core.h>

#include <cmath>

#include "PulsarPresetData.h"
#include "engine/Utilities.h"

namespace vibesynth {

namespace {

ParseResult fail (PatchStatus status, const juce::String& message)
{
    ParseResult r;
    r.status = status;
    r.error = message.toStdString();
    return r;
}

juce::String kilobytes (size_t bytes)
{
    return juce::String ((bytes + 1023) / 1024) + " KB";
}

bool isNumber (const juce::var& v)
{
    return v.isInt() || v.isInt64() || v.isDouble();
}

bool isBlank (const std::string& s)
{
    for (const char c : s)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            return false;
    return true;
}

// Reads code.<key> into out. Absent = empty; anything but a string is an
// error; larger than kMaxSectionBytes is TooLarge.
bool readSection (const juce::var& code, const char* key, std::string& out, ParseResult& err)
{
    const juce::var v = code.getProperty (key, juce::var());
    if (v.isVoid() || v.isUndefined())
    {
        out.clear();
        return true;
    }
    if (! v.isString())
    {
        err = fail (PatchStatus::BadField, "code." + juce::String (key) + " must be a string");
        return false;
    }
    out = v.toString().toStdString();
    if (out.size() > Patch::kMaxSectionBytes)
    {
        err = fail (PatchStatus::TooLarge, "code." + juce::String (key) + " too large ("
                                               + kilobytes (out.size()) + ", max "
                                               + kilobytes (Patch::kMaxSectionBytes) + ")");
        return false;
    }
    return true;
}

} // namespace

const char* patchStatusName (PatchStatus s) noexcept
{
    switch (s)
    {
        case PatchStatus::Ok:           return "ok";
        case PatchStatus::BadJson:      return "bad_json";
        case PatchStatus::BadField:     return "bad_field";
        case PatchStatus::MissingVoice: return "missing_voice";
        case PatchStatus::TooLarge:     return "oversize";
        case PatchStatus::CompileError: return "compile_error";
        case PatchStatus::NonFinite:    return "nan";
        case PatchStatus::TooLoud:      return "loud";
        case PatchStatus::DcOffset:     return "dc";
        case PatchStatus::Silent:       return "silent";
        case PatchStatus::TooSlow:      return "slow";
    }
    return "unknown";
}

ParseResult parsePatch (const std::string& text)
{
    if (text.size() > Patch::kMaxFileBytes)
        return fail (PatchStatus::TooLarge, "patch file too large (" + kilobytes (text.size())
                                                + ", max " + kilobytes (Patch::kMaxFileBytes) + ")");

    juce::var root;
    const auto parsed = juce::JSON::parse (juce::String::fromUTF8 (text.data(), (int) text.size()), root);
    if (parsed.failed())
        return fail (PatchStatus::BadJson, "invalid JSON: " + parsed.getErrorMessage());
    if (! root.isObject())
        return fail (PatchStatus::BadJson, "invalid JSON: expected an object");

    ParseResult r;
    Patch& p = r.patch;

    const juce::var version = root.getProperty ("version", juce::var());
    if (! isNumber (version))
        return fail (PatchStatus::BadField, "missing \"version\"");
    if (! juce::exactlyEqual (static_cast<double> (version), static_cast<double> (Patch::kVersion)))
        return fail (PatchStatus::BadField, "unsupported version " + version.toString()
                                                + " (expected " + juce::String (Patch::kVersion) + ")");
    p.version = Patch::kVersion;

    const juce::var code = root.getProperty ("code", juce::var());
    if (! code.isObject() || ! code.hasProperty ("voice"))
        return fail (PatchStatus::MissingVoice, "missing code.voice");
    if (! readSection (code, "voice", p.voice, r)
        || ! readSection (code, "init", p.init, r)
        || ! readSection (code, "master", p.master, r))
        return r;
    if (isBlank (p.voice))
        return fail (PatchStatus::MissingVoice, "code.voice is empty");

    p.name = root.getProperty ("name", "").toString().trim().toStdString();
    if (p.name.empty())
        p.name = "Untitled";
    p.prompt = root.getProperty ("prompt", "").toString().toStdString();

    const juce::var macros = root.getProperty ("macros", juce::var());
    if (! macros.isVoid() && ! macros.isUndefined())
    {
        const auto* list = macros.getArray();
        if (list == nullptr)
            return fail (PatchStatus::BadField, "\"macros\" must be a list");
        if (list->size() > Patch::kMaxMacros)
            return fail (PatchStatus::BadField, "too many macros (" + juce::String (list->size())
                                                    + ", max " + juce::String (Patch::kMaxMacros) + ")");
        for (const auto& m : *list)
        {
            if (! m.isObject())
                return fail (PatchStatus::BadField, "each macro must be {\"label\", \"default\"}");
            MacroSpec spec;
            spec.label = m.getProperty ("label", "").toString().toStdString();
            const juce::var def = m.getProperty ("default", juce::var());
            double d = isNumber (def) ? static_cast<double> (def) : 0.5;
            if (! isFiniteBitsD (d))
                d = 0.5;
            spec.defaultValue = quantisePatchNumber (juce::jlimit (0.0, 1.0, d));
            p.macros.push_back (spec);
        }
    }

    const juce::var mode = root.getProperty ("voiceMode", juce::var());
    if (! mode.isVoid() && ! mode.isUndefined())
    {
        const auto m = mode.toString();
        if (m != "poly" && m != "mono")
            return fail (PatchStatus::BadField, "voiceMode must be \"poly\" or \"mono\"");
        p.voiceMode = m.toStdString();
    }

    const juce::var graph = root.getProperty ("graph", juce::var());
    if (! graph.isVoid() && ! graph.isUndefined())
        p.graphJson = juce::JSON::toString (graph, juce::JSON::FormatOptions{}
                                                       .withSpacing (juce::JSON::Spacing::none))
                          .toStdString();

    const juce::var trim = root.getProperty ("trim", juce::var());
    double t = isNumber (trim) ? static_cast<double> (trim) : 1.0;
    if (! isFiniteBitsD (t) || t <= 0.0 || t > 1.0)
        t = 1.0;
    p.trim = juce::jmax (1.0e-6, quantisePatchNumber (t));

    return r;
}

std::string serializePatch (const Patch& p)
{
    auto* root = new juce::DynamicObject();
    const juce::var rootVar (root);

    root->setProperty ("version", p.version);
    root->setProperty ("name", juce::String (p.name));
    root->setProperty ("prompt", juce::String (p.prompt));

    juce::Array<juce::var> macros;
    for (const auto& m : p.macros)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("label", juce::String (m.label));
        o->setProperty ("default", m.defaultValue);
        macros.add (juce::var (o));
    }
    root->setProperty ("macros", macros);
    root->setProperty ("voiceMode", juce::String (p.voiceMode));

    if (! p.graphJson.empty())
    {
        juce::var graph;
        if (juce::JSON::parse (juce::String (p.graphJson), graph).wasOk())
            root->setProperty ("graph", graph);
    }

    auto* code = new juce::DynamicObject();
    code->setProperty ("init", juce::String (p.init));
    code->setProperty ("voice", juce::String (p.voice));
    code->setProperty ("master", juce::String (p.master));
    root->setProperty ("code", juce::var (code));
    root->setProperty ("trim", p.trim);

    return juce::JSON::toString (rootVar, juce::JSON::FormatOptions{}
                                              .withSpacing (juce::JSON::Spacing::multiLine)
                                              .withMaxDecimalPlaces (6))
               .toStdString()
         + "\n";
}

// ---------------------------------------------------------------------------
// Built-in presets. Order is the host program order; index 0 is the
// default patch.

namespace {

struct BuiltinEntry
{
    const char* file;
    const char* resource; // BinaryData resource name
};

constexpr BuiltinEntry kBuiltins[] = {
    { "reese.pulsar", "reese_pulsar" },
    { "sub.pulsar",   "sub_pulsar" },
    { "pluck.pulsar", "pluck_pulsar" },
};

constexpr int kNumBuiltins = static_cast<int> (sizeof (kBuiltins) / sizeof (kBuiltins[0]));

} // namespace

int numBuiltinPresets() noexcept
{
    return kNumBuiltins;
}

const char* builtinPresetFile (int index) noexcept
{
    return index >= 0 && index < kNumBuiltins ? kBuiltins[index].file : "";
}

std::string builtinPresetText (int index)
{
    if (index < 0 || index >= kNumBuiltins)
        return {};
    int size = 0;
    const char* data = PulsarPresetData::getNamedResource (kBuiltins[index].resource, size);
    return data != nullptr ? std::string (data, (size_t) size) : std::string();
}

std::string builtinPresetName (int index)
{
    static const std::vector<std::string> names = []
    {
        std::vector<std::string> n;
        for (int i = 0; i < kNumBuiltins; ++i)
        {
            const ParseResult r = parsePatch (builtinPresetText (i));
            n.push_back (r.ok() ? r.patch.name : std::string (kBuiltins[i].file));
        }
        return n;
    }();
    return index >= 0 && index < kNumBuiltins ? names[(size_t) index] : std::string();
}

} // namespace vibesynth
