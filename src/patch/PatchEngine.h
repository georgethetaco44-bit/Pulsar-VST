#pragma once

// M3 spike: the EEL2 voice engine (SPEC §4.2, §13 M3).
//
// One `EelVoice` owns one EEL2 VM compiled from a hardcoded program. The
// audio thread writes the bound variables and calls `renderSample()` — all
// VM creation and compilation happens off the audio thread (constructor /
// `prepare`). Linkage: `vibesynth_engine` static lib, shared by the plugin
// and `vibesynth_cli` so the CLI tests exactly the code the plugin runs.
//
// Real-time rules (SPEC §11): `renderSample()` performs no allocation,
// locks, logging, I/O, exceptions, or std::function. NaN/Inf checks use
// bit-pattern tests, never std::isnan (survives any fast-math flags).

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

// EEL2 public API (vendored in third_party/WDL; see VERSION for the pin).
// ns-eel-addfuncs.h declares the pproc helpers (e.g. NSEEL_PProc_THIS)
// used when registering native functions.
#include "WDL/eel2/ns-eel.h"
#include "WDL/eel2/ns-eel-addfuncs.h"

namespace vibesynth {

// ---------------------------------------------------------------------------
// Bit-pattern float checks (SPEC §11: survive -ffast-math).

inline bool isFiniteBits (double v) noexcept
{
    uint64_t bits = 0;
    static_assert (sizeof (bits) == sizeof (v), "double must be 64-bit");
    std::memcpy (&bits, &v, sizeof (v));
    // Exponent all-ones (0x7FF...) => Inf or NaN, regardless of mantissa.
    return ((bits >> 52) & 0x7FFu) != 0x7FFu;
}

// NaN/Inf -> 0, then hard-clamp to [-1, 1]. Usable on the audio thread.
inline float sanitizeAndClamp (double v) noexcept
{
    if (! isFiniteBits (v))
        return 0.0f;
    if (v > 1.0)
        return 1.0f;
    if (v < -1.0)
        return -1.0f;
    return static_cast<float> (v);
}

// ---------------------------------------------------------------------------
// Process-wide EEL2 setup.
//
// ensureEelInitialised() runs exactly once (std::call_once): it probes JIT
// usability (return codes of mmap/mprotect or VirtualAlloc/VirtualProtect
// only — see below), calls NSEEL_init(), and registers the M3 native
// function table. It must run before any VM is created and is never
// repeated, because EEL2's function table is global (NSEEL_init itself
// calls NSEEL_quit, which clears it).
//
// The JIT probe checks the executable-memory path WDL itself uses
// (mmap RW -> mprotect RX; VirtualAlloc RW -> VirtualProtect RX) by return
// code only. It deliberately does NOT write or execute a stub: if the
// permission toggle reports success but execution would still fault, running
// a stub would crash just like the real code, proving nothing. Failed
// return codes are the catchable case, and that is exactly what we catch.
// Portable (EEL_TARGET_PORTABLE) builds skip the probe: no exec pages needed.
bool isJitUsable();          // false in portable builds or if probe failed
bool ensureEelInitialised(); // call once per process before creating voices

// Which execution mode was compiled in: "JIT (x86_64)", "JIT (aarch64)",
// "JIT (other-arch)" or "portable".
const char* eelExecutionMode();

// ---------------------------------------------------------------------------
// Per-voice state that our one native function reaches through EEL2's
// per-VM custom-this pointer (NSEEL_VM_SetCustomFuncThis). This is the SPEC
// §4.2 mechanism under test: each VM's function calls must see only that
// voice's context.
struct EelVoiceContext
{
    static constexpr int numSlots = 16;
    double phase[numSlots] = {}; // one phase accumulator per slot
    double sampleRate = 48000.0;
};

// ---------------------------------------------------------------------------
// One voice: one EEL2 VM, one compiled program, four bound variables.
// All creation/compilation in the constructor (off audio thread).
// renderSample() is the only audio-thread entry point.

class EelVoice
{
public:
    // Compiles `program` immediately. Check isValid(); on failure the voice
    // is silent and compileError() holds the EEL2 error string.
    explicit EelVoice (const char* program, double sampleRate = 48000.0);
    ~EelVoice();

    EelVoice (const EelVoice&) = delete;
    EelVoice& operator= (const EelVoice&) = delete;

    bool isValid() const noexcept { return code_ != nullptr; }
    const std::string& compileError() const noexcept { return compileError_; }

    void setSampleRate (double sampleRate) noexcept; // updates `srate` var
    void resetOscillator() noexcept;                 // zeroes phases

    // Audio thread: writes freq/gate, executes, returns clamped `out`.
    // Never allocates/locks/logs; failure (invalid VM) returns silence.
    float renderSample (double freq, double gate) noexcept;

private:
    NSEEL_VMCTX vm_ = nullptr;
    NSEEL_CODEHANDLE code_ = nullptr;
    void* gram_ = nullptr; // per-VM mem[] context (isolates voices)
    EelVoiceContext context_;
    EEL_F* freq_ = nullptr;
    EEL_F* gate_ = nullptr;
    EEL_F* srate_ = nullptr;
    EEL_F* out_ = nullptr;
    std::string compileError_;
};

} // namespace vibesynth
