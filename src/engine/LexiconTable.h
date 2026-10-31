#pragma once

// M8: the registered lexicon as data (SPEC §5.2, §6.4). One table drives
// registerLexiconFunctions(), the lexicon lint (patch/LexiconLint.h) and
// the smoketest that checks resources/prompts/api_reference.md against
// what is really registered, so the three can't drift apart.
//
// M10 (SPEC §9.6): every row also carries the node inspector's metadata,
// per argument (display name, kind, range) and per function (the node it
// meters as, bypass behaviour, trace style). The override layer, the trace
// capture, the inspector and --bake all read only this table, so a new
// lexicon function gets inspector support by adding its row.
//
// JUCE-free and EEL2-free; LexiconReg.cpp pairs each entry with its
// wrapper function.

#include <cstdint>

namespace vibesynth {

enum class LexiconScope
{
    Both,   // voice and master (the master has its own VoiceRack, M6)
    Master  // bus_*: master only (they return 0 in a voice VM)
};

// What an argument is, for the inspector (SPEC §9.6). Only Norm, Hz, Beats
// and Choice get a knob; the others are wiring the code owns.
enum class ArgKind : uint8_t
{
    Audio,   // a signal input (in, exciter, l, r, t): not editable
    Slot,    // the slot literal: not editable
    Norm,    // 0-1, mapped inside the wrapper
    Hz,      // a frequency in Hz; Offset works in semitones
    Beats,   // a length in beats (M12: mod_lfo)
    Choice,  // an integer choice in [lo, hi] (chaos axis, bytebeat mode)
    Gate     // a gate the code drives: not editable
};

inline bool isEditableArg (ArgKind k) noexcept
{
    return k == ArgKind::Norm || k == ArgKind::Hz || k == ArgKind::Beats || k == ArgKind::Choice;
}

// Bypass (SPEC §9.6): effects/filters/utilities pass their input through,
// sources output 0, envelopes have none. ForceArg runs the function with
// one argument fixed (bus_chorus/bus_reverb: mix 0, so both channels come
// out dry and the _r companion stays consistent).
enum class BypassKind : uint8_t { None, PassArg, Zero, ForceArg };

// How the node's output is traced for the UI (SPEC §9.6). ByRate: control
// when the rate argument (traceArg) is below ~20 Hz, else audio.
enum class TraceKind : uint8_t { None, Audio, Control, ByRate };

constexpr int kMaxLexiconArgs = 6;

struct LexiconArg
{
    const char* name = "";    // as in `params` ("cutoff")
    const char* label = "";   // the inspector's knob name ("Cutoff")
    ArgKind kind = ArgKind::Audio;
    double lo = 0.0;          // the knob's range (Replace values, map min/max)
    double hi = 1.0;
};

struct LexiconEntry
{
    const char* name;
    const char* params;   // "slot, hz": the signature as documented
    int numArgs;          // exact argument count
    int numSlots;         // > 0: the first argument is a slot, 0..numSlots-1; 0: no slot
    LexiconScope scope;

    // M10. `module` is the ModuleId the call meters as (-1: not a node,
    // e.g. noise). One function per module is `primary`: its arguments are
    // the node's knobs and its output is the node's trace; companions
    // (util_xover_hi, bus_*_r) only honour the node's bypass.
    int module = -1;
    bool primary = false;
    BypassKind bypass = BypassKind::None;
    int bypassArg = 0;        // PassArg: the argument returned; ForceArg: the argument fixed
    double bypassValue = 0.0; // ForceArg: its value
    TraceKind trace = TraceKind::None;
    int traceArg = 0;         // ByRate: the rate argument
    LexiconArg args[kMaxLexiconArgs] = {};
};

int numLexiconEntries() noexcept;
const LexiconEntry& lexiconEntry (int index) noexcept;
// Case-insensitive, as EEL2 resolves names. Null if not registered.
const LexiconEntry* findLexiconEntry (const char* name) noexcept;
// M10: the primary entry of a module (its knobs), or null.
const LexiconEntry* primaryLexiconEntry (int module) noexcept;
// M10: the argument index of `name` in `e`, or -1.
int lexiconArgIndex (const LexiconEntry& e, const char* name) noexcept;
// M10: true when this call's output is control-rate (a rolling trace)
// given its (effective) arguments.
bool lexiconTraceIsControl (const LexiconEntry& e, const double* args) noexcept;

} // namespace vibesynth
