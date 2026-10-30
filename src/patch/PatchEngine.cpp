// M4 implementation. EEL2 plumbing (host stubs, JIT probe, once-init)
// is unchanged from M3; the native function is now the full lexicon
// (engine/LexiconReg.cpp). This file owns EelVoice + EelMaster.

#include "patch/PatchEngine.h"

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

// M4 — `namespace vibesynth`; every helper that is not part of the
// PatchEngine.h API lives in an anonymous namespace (internal linkage).
// The EEL2 host stubs are extern "C" and must stay external.
namespace vibesynth {

// ---------------------------------------------------------------------------
// Host stubs required by EEL2 (ns-eel.h). Backed by a real recursive mutex:
// M5 moves compilation to a worker thread and a DAW can host several Pulsar
// instances, so the global function table / RAM-block paths they guard need
// real mutual exclusion. Recursive, because EEL2 can re-enter these paths
// (e.g. allocation during free).

namespace {

std::recursive_mutex& eelGlobalMutex()
{
    static std::recursive_mutex m;
    return m;
}

} // namespace

extern "C" void NSEEL_HOSTSTUB_EnterMutex() { eelGlobalMutex().lock(); }
extern "C" void NSEEL_HOSTSTUB_LeaveMutex() { eelGlobalMutex().unlock(); }

// ---------------------------------------------------------------------------
// JIT probe: exercise exactly WDL's executable-page path
// (mmap RW -> mprotect RX; VirtualAlloc RW -> VirtualProtect RX) and check
// the return codes only. No code is written to the page and nothing is
// executed: a host that reports success but faults on execute can't be
// detected this way, and crashing in the probe would prove nothing.

namespace {

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
    registerLexiconFunctions();
} // initialiseEelOnce

} // namespace

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

bool isJitUsable()
{
    return ensureEelInitialised() && g_jitUsable;
}
} // namespace vibesynth

// NSEEL_init/register live in the block above.
namespace vibesynth {
namespace {

bool setupVm (NSEEL_VMCTX vm, void** gramSlot, VmRackContext* rackCtx,
              int ramSlots)
{
    NSEEL_VM_SetGRAM (vm, gramSlot);
    NSEEL_VM_setramsize (vm, ramSlots);
    // Pre-allocate ALL RAM blocks now, off the audio thread (SPEC §4.2).
    NSEEL_VM_preallocram (vm, -1);
    NSEEL_VM_SetCustomFuncThis (vm, rackCtx);
    return true;
}

EEL_F* mustRegVar (NSEEL_VMCTX vm, const char* name, bool& ok) noexcept
{
    EEL_F* v = NSEEL_VM_regvar (vm, name);
    if (v == nullptr)
        ok = false;
    else
        *v = 0.0;
    return v;
}

const char* kMacroNames[8] = {
    "m1", "m2", "m3", "m4", "m5", "m6", "m7", "m8"
};

} // namespace
} // namespace vibesynth

namespace vibesynth { // (methods block: EelVoice/EelMaster/eelExecutionMode)

EelVoice::EelVoice (const PatchStrings& patch, double sampleRate,
                    uint32_t voiceSeed)
{
    rackStorage_.init (sampleRate > 0.0 ? sampleRate : 48000.0, voiceSeed);
    rackCtx_.voice = &rackStorage_;
    rackCtx_.master = nullptr;

    if (! ensureEelInitialised() || ! isJitUsable())
    {
        compileError_ = "EEL2 JIT unavailable; voice silent";
        return;
    }

    vm_ = NSEEL_VM_alloc();
    if (vm_ == nullptr)
    {
        compileError_ = "NSEEL_VM_alloc failed";
        return;
    }

    setupVm (vm_, &gram_, &rackCtx_, 65536);

    bool ok = true;
    freq_ = mustRegVar (vm_, "freq", ok);
    note_ = mustRegVar (vm_, "note", ok);
    vel_ = mustRegVar (vm_, "vel", ok);
    gate_ = mustRegVar (vm_, "gate", ok);
    trig_ = mustRegVar (vm_, "trig", ok);
    srate_ = mustRegVar (vm_, "srate", ok);
    for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
        macro_[i] = mustRegVar (vm_, kMacroNames[i], ok);
    out_ = mustRegVar (vm_, "out", ok);
    (void) mustRegVar (vm_, "fb1", ok);
    (void) mustRegVar (vm_, "fb2", ok);
    (void) mustRegVar (vm_, "prev", ok);
    if (! ok)
    {
        compileError_ = "NSEEL_VM_regvar failed";
        NSEEL_VM_free (vm_);
        vm_ = nullptr;
        return;
    }
    *srate_ = rackStorage_.sampleRate;
    for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
        *macro_[i] = 0.5;

    initCode_ = NSEEL_code_compile (vm_, patch.init != nullptr ? patch.init : "", 0);
    voiceCode_ = NSEEL_code_compile (vm_, patch.voice != nullptr ? patch.voice : "", 0);
    if (initCode_ == nullptr || voiceCode_ == nullptr)
    {
        const char* err = NSEEL_code_getcodeerror (vm_);
        compileError_ = (err != nullptr && *err != '\0') ? err : "compile failed";
        if (initCode_ != nullptr) { NSEEL_code_free (initCode_); initCode_ = nullptr; }
        if (voiceCode_ != nullptr) { NSEEL_code_free (voiceCode_); voiceCode_ = nullptr; }
        NSEEL_VM_free (vm_);
        vm_ = nullptr;
        freq_ = note_ = vel_ = gate_ = trig_ = srate_ = out_ = nullptr;
        for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
            macro_[i] = nullptr;
    }
}

EelVoice::~EelVoice()
{
    if (initCode_ != nullptr) { NSEEL_code_free (initCode_); initCode_ = nullptr; }
    if (voiceCode_ != nullptr) { NSEEL_code_free (voiceCode_); voiceCode_ = nullptr; }
    if (vm_ != nullptr) { NSEEL_VM_free (vm_); vm_ = nullptr; }
    if (gram_ != nullptr) { NSEEL_VM_FreeGRAM (&gram_); gram_ = nullptr; }
}

// SPEC §4.3: trig is 1 on the note-on sample only. It is 1 while init runs
// and stays 1 into the next renderSample() (the note-on sample), which
// clears it after executing the voice code.
void EelVoice::noteOn (double freq, double note, double vel) noexcept
{
    lifetime.noteOn();
    if (vm_ == nullptr || initCode_ == nullptr || freq_ == nullptr)
        return;
    *freq_ = freq;
    if (note_ != nullptr) *note_ = note;
    if (vel_ != nullptr) *vel_ = vel;
    if (gate_ != nullptr) *gate_ = 1.0;
    if (trig_ != nullptr) *trig_ = 1.0;
    NSEEL_code_execute (initCode_);
    if (trig_ != nullptr) *trig_ = 1.0; // even if init wrote it
    if (gate_ != nullptr) *gate_ = 1.0;
}

float EelVoice::renderSample (double freq, double gate, double note, double vel,
                              const double macros[MacroSmoother::kNumMacros]) noexcept
{
    if (voiceCode_ == nullptr || freq_ == nullptr || out_ == nullptr)
        return 0.0f;
    *freq_ = freq;
    if (gate_ != nullptr) *gate_ = gate;
    if (note_ != nullptr) *note_ = note;
    if (vel_ != nullptr) *vel_ = vel;
    if (srate_ != nullptr) *srate_ = rackStorage_.sampleRate;
    for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
        if (macro_[i] != nullptr)
            *macro_[i] = macros[i];
    NSEEL_code_execute (voiceCode_);
    if (trig_ != nullptr) *trig_ = 0.0;
    return sanitizeAndClamp (*out_);
}

// ---------------------------------------------------------------------------
// EelMaster methods (same namespace vibesynth as EelVoice above).

EelMaster::EelMaster (const PatchStrings& patch, double sampleRate)
{
    // Own the FDN delay memory: worst case at this rate, then init().
    const double scale = (sampleRate / 48000.0) * 1.5;
    int need = 0;
    for (int l = 0; l < FdnReverbState::kNumLines; ++l)
        need += static_cast<int> (FdnReverbState::kBaseLen[l] * scale) + 64;
    reverbStorage_ = new (std::nothrow) float[(size_t) (need > 0 ? need : 1)];
    reverbCapacity_ = (reverbStorage_ != nullptr) ? need : 0;

    rackStorage_.reverbStorage = reverbStorage_;
    rackStorage_.reverbCapacity = reverbCapacity_;
    rackStorage_.init (sampleRate > 0.0 ? sampleRate : 48000.0);
    rackCtx_.voice = nullptr;
    rackCtx_.master = &rackStorage_;

    if (! ensureEelInitialised() || ! isJitUsable())
    {
        compileError_ = "EEL2 JIT unavailable; master silent";
        return;
    }

    vm_ = NSEEL_VM_alloc();
    if (vm_ == nullptr)
    {
        compileError_ = "NSEEL_VM_alloc failed (master)";
        return;
    }

    setupVm (vm_, &gram_, &rackCtx_, 65536);

    bool ok = true;
    inl_ = mustRegVar (vm_, "inl", ok);
    inr_ = mustRegVar (vm_, "inr", ok);
    outl_ = mustRegVar (vm_, "outl", ok);
    outr_ = mustRegVar (vm_, "outr", ok);
    srate_ = mustRegVar (vm_, "srate", ok);
    for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
        macro_[i] = mustRegVar (vm_, kMacroNames[i], ok);
    if (! ok)
    {
        compileError_ = "NSEEL_VM_regvar failed (master)";
        NSEEL_VM_free (vm_);
        vm_ = nullptr;
        return;
    }
    *srate_ = rackStorage_.sampleRate;
    for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
        *macro_[i] = 0.5;

    masterCode_ = NSEEL_code_compile (vm_, patch.master != nullptr ? patch.master : "", 0);
    if (masterCode_ == nullptr)
    {
        const char* err = NSEEL_code_getcodeerror (vm_);
        compileError_ = (err != nullptr && *err != '\0') ? err : "compile failed";
        NSEEL_VM_free (vm_);
        vm_ = nullptr;
        inl_ = inr_ = outl_ = outr_ = srate_ = nullptr;
        for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
            macro_[i] = nullptr;
    }
}

EelMaster::~EelMaster()
{
    if (masterCode_ != nullptr) { NSEEL_code_free (masterCode_); masterCode_ = nullptr; }
    if (vm_ != nullptr) { NSEEL_VM_free (vm_); vm_ = nullptr; }
    if (gram_ != nullptr) { NSEEL_VM_FreeGRAM (&gram_); gram_ = nullptr; }
    delete[] reverbStorage_;
    reverbStorage_ = nullptr;
}

void EelMaster::renderSample (float inL, float inR, float& outL, float& outR,
                              const double macros[MacroSmoother::kNumMacros]) noexcept
{
    if (masterCode_ == nullptr || inl_ == nullptr || outl_ == nullptr)
    {
        outL = sanitizeAndClamp (inL);
        outR = sanitizeAndClamp (inR);
        return;
    }
    *inl_ = inL;
    if (inr_ != nullptr) *inr_ = inR;
    if (srate_ != nullptr) *srate_ = rackStorage_.sampleRate;
    for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
        if (macro_[i] != nullptr)
            *macro_[i] = macros[i];
    if (outl_ != nullptr) *outl_ = inL;
    if (outr_ != nullptr) *outr_ = inR;
    NSEEL_code_execute (masterCode_);
    outL = sanitizeAndClamp (*outl_);
    outR = (outr_ != nullptr) ? sanitizeAndClamp (*outr_) : outL;
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

} // namespace vibesynth
