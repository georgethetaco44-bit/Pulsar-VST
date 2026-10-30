// M3 spike implementation. See PatchEngine.h for the contract.
// Real-time rules (SPEC §11) apply to renderSample(): nothing there may
// allocate, lock, log, throw, or do I/O.

#include "PatchEngine.h"

#include <cmath>
#include <mutex>

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace vibesynth {
namespace {

// ---------------------------------------------------------------------------
// Host stubs required by EEL2 (ns-eel.h). Backed by a real recursive mutex:
// M5 moves compilation to a worker thread and a DAW can host several Pulsar
// instances, so the global function table / RAM-block paths they guard need
// real mutual exclusion. Recursive, because EEL2 can re-enter these paths
// (e.g. allocation during free).

std::recursive_mutex& eelGlobalMutex()
{
    static std::recursive_mutex m;
    return m;
}

} // namespace

extern "C" void NSEEL_HOSTSTUB_EnterMutex() { eelGlobalMutex().lock(); }
extern "C" void NSEEL_HOSTSTUB_LeaveMutex() { eelGlobalMutex().unlock(); }

namespace {

// ---------------------------------------------------------------------------
// osc_sine(slot, hz): our ONE registered native function. Receives the
// per-voice context through the VM's custom-this pointer (the SPEC §4.2
// mechanism). Signature mandated by NSEEL_addfunc_varparm.

EEL_F NSEEL_CGEN_CALL oscSineNative (void* ctx, INT_PTR np, EEL_F** parms)
{
    auto* vc = static_cast<EelVoiceContext*> (ctx);
    if (vc == nullptr || np < 2 || parms == nullptr
        || parms[0] == nullptr || parms[1] == nullptr)
        return 0.0;

    int slot = static_cast<int> (*parms[0]);
    if (slot < 0)
        slot = 0;
    else if (slot >= EelVoiceContext::numSlots)
        slot = EelVoiceContext::numSlots - 1;

    const double hz = *parms[1];
    const double sr = vc->sampleRate > 0.0 ? vc->sampleRate : 48000.0;

    double& phase = vc->phase[slot];
    phase += hz / sr; // phase in cycles; wraps below
    phase -= std::floor (phase);

    return std::sin (6.28318530717958647692 * phase);
}


// ---------------------------------------------------------------------------
// JIT probe (amended plan §1): exercise exactly WDL's executable-page path
// (mmap RW -> mprotect RX; VirtualAlloc RW -> VirtualProtect RX) and check
// the return codes only. No code is written to the page and nothing is
// executed: a host that reports success but faults on execute can't be
// detected this way, and crashing in the probe would prove nothing. We catch
// the catchable case — failed return codes (hardened runtime without
// allow-jit) — and fail safe to silence, never to a crash.

bool probeJitUsable()
{
#ifdef EEL_TARGET_PORTABLE
    return false;
#elif defined(_WIN32)
    SYSTEM_INFO inf{};
    GetSystemInfo (&inf);
    const SIZE_T page = inf.dwPageSize > 0 ? inf.dwPageSize : 4096;

    void* p = VirtualAlloc (nullptr, page, MEM_COMMIT, PAGE_READWRITE);
    if (p == nullptr)
        return false;
    DWORD old = 0;
    const BOOL ok = VirtualProtect (p, page, PAGE_EXECUTE_READ, &old);
    VirtualFree (p, 0, MEM_RELEASE);
    return ok != 0;
#else
    const long ps = sysconf (_SC_PAGESIZE);
    const size_t page = ps > 0 ? static_cast<size_t> (ps) : 4096;

    void* p = mmap (nullptr, page, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return false;
    const int rc = mprotect (p, page, PROT_READ | PROT_EXEC);
    munmap (p, page);
    return rc == 0;
#endif
}

std::once_flag& eelOnceFlag()
{
    static std::once_flag f;
    return f;
}

bool g_jitUsable = true; // set by the once-init; read-only afterwards

void initialiseEelOnce()
{
    g_jitUsable = probeJitUsable();
#ifdef EEL_TARGET_PORTABLE
    (void) g_jitUsable;
    g_jitUsable = false;
#endif
    if (NSEEL_init() != 0)
    {
        g_jitUsable = false;
        return;
    }
    // Register exactly once, before any VM exists; never again, never quit.
    NSEEL_addfunc_varparm ("osc_sine", 2, NSEEL_PProc_THIS, &oscSineNative);
}

} // namespace

bool isJitUsable()
{
    return ensureEelInitialised() && g_jitUsable;
}

bool ensureEelInitialised()
{
    try
    {
        std::call_once (eelOnceFlag(), initialiseEelOnce);
    }
    catch (...)
    {
        return false;
    }
    return true;
}

const char* eelExecutionMode()
{
#ifdef EEL_TARGET_PORTABLE
    return "portable";
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
    return "JIT (aarch64)";
#elif defined(__x86_64__) || defined(_M_X64)
    return "JIT (x86_64)";
#else
    return "JIT (other-arch)";
#endif
}

// ---------------------------------------------------------------------------

EelVoice::EelVoice (const char* program, double sampleRate)
{
    context_.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;

    if (! ensureEelInitialised() || ! isJitUsable())
    {
        compileError_ = "EEL2 JIT unavailable on this machine (exec pages "
                        "denied or NSEEL_init failed); voice silent";
        return;
    }

    vm_ = NSEEL_VM_alloc();
    if (vm_ == nullptr)
    {
        compileError_ = "NSEEL_VM_alloc failed";
        return;
    }

    // Per-VM mem[] context: voices never share delay-line/table memory.
    NSEEL_VM_SetGRAM (vm_, &gram_);
    // Cap VM RAM (SPEC §4: rule it out as a blow-up vector). Small fixed
    // size for the spike; real sizing comes with M5's prealloc work.
    NSEEL_VM_setramsize (vm_, 65536);
    NSEEL_VM_SetCustomFuncThis (vm_, &context_);

    freq_ = NSEEL_VM_regvar (vm_, "freq");
    gate_ = NSEEL_VM_regvar (vm_, "gate");
    srate_ = NSEEL_VM_regvar (vm_, "srate");
    out_ = NSEEL_VM_regvar (vm_, "out");
    if (freq_ == nullptr || gate_ == nullptr || srate_ == nullptr
        || out_ == nullptr)
    {
        compileError_ = "NSEEL_VM_regvar failed";
        NSEEL_VM_free (vm_);
        vm_ = nullptr;
        return;
    }
    *freq_ = 0.0;
    *gate_ = 0.0;
    *srate_ = context_.sampleRate;
    *out_ = 0.0;

    code_ = NSEEL_code_compile (vm_, program != nullptr ? program : "", 0);
    if (code_ == nullptr)
    {
        const char* err = NSEEL_code_getcodeerror (vm_);
        compileError_ = (err != nullptr && *err != '\0')
            ? err : "NSEEL_code_compile failed (unknown error)";
        NSEEL_VM_free (vm_);
        vm_ = nullptr;
        freq_ = gate_ = srate_ = out_ = nullptr;
    }
}

EelVoice::~EelVoice()
{
    // Off audio thread only (SPEC §4: construction/destruction never runs on
    // the audio thread; the audio thread only calls renderSample).
    if (code_ != nullptr)
    {
        NSEEL_code_free (code_);
        code_ = nullptr;
    }
    if (vm_ != nullptr)
    {
        NSEEL_VM_free (vm_);
        vm_ = nullptr;
    }
    if (gram_ != nullptr)
    {
        NSEEL_VM_FreeGRAM (&gram_);
        gram_ = nullptr;
    }
}

void EelVoice::setSampleRate (double sampleRate) noexcept
{
    context_.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    if (srate_ != nullptr)
        *srate_ = context_.sampleRate;
}

void EelVoice::resetOscillator() noexcept
{
    for (double& p : context_.phase)
        p = 0.0;
}

float EelVoice::renderSample (double freq, double gate) noexcept
{
    if (code_ == nullptr || freq_ == nullptr || out_ == nullptr)
        return 0.0f; // invalid VM: silence, never a crash

    *freq_ = freq;
    *gate_ = gate;
    NSEEL_code_execute (code_);
    return sanitizeAndClamp (*out_);
}

} // namespace vibesynth

const char* eelExecutionMode()
{
#ifdef EEL_TARGET_PORTABLE
    return "portable";
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
    return "JIT (aarch64)";
#elif defined(__x86_64__) || defined(_M_X64)
    return "JIT (x86_64)";
#else
    return "JIT (other-arch)";
#endif
}
