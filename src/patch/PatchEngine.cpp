// EEL2 plumbing (host stubs, JIT probe, once-init) is unchanged from M3;
// the native functions are the full lexicon (engine/LexiconReg.cpp). This
// file owns EelVoice + EelMaster.

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

// True when `code` has no statements: empty, whitespace, or comments only
// (EEL2 // and /* */ comments). Such init/master sections are skipped.
bool isBlankCode (const char* code) noexcept
{
    if (code == nullptr)
        return true;
    const char* p = code;
    while (*p != '\0')
    {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ';')
        {
            ++p;
        }
        else if (p[0] == '/' && p[1] == '/')
        {
            while (*p != '\0' && *p != '\n')
                ++p;
        }
        else if (p[0] == '/' && p[1] == '*')
        {
            p += 2;
            while (*p != '\0' && ! (p[0] == '*' && p[1] == '/'))
                ++p;
            if (*p != '\0')
                p += 2;
        }
        else
        {
            return false;
        }
    }
    return true;
}

std::string codeError (NSEEL_VMCTX vm)
{
    const char* err = NSEEL_code_getcodeerror (vm);
    return (err != nullptr && *err != '\0') ? std::string (err) : std::string ("compile failed");
}

// Every variable the compiled code can touch (SPEC §7.2 reset list).
// Called once after compiling, off the audio thread.
void collectVars (NSEEL_VMCTX vm, std::vector<EEL_F_PTR>& out)
{
    out.clear();
    NSEEL_VM_enumallvars (vm, [] (const char*, EEL_F* val, void* ctx) -> int
                          {
                              if (val != nullptr)
                                  static_cast<std::vector<EEL_F_PTR>*> (ctx)->push_back (val);
                              return 1;
                          }, &out);
}

// Zeroes mem[] through the no-alloc accessor. Every block is preallocated
// (setupVm), so this never allocates; an absent block is skipped.
void zeroVmRam (NSEEL_VMCTX vm) noexcept
{
    unsigned int offs = 0;
    const unsigned int slots = static_cast<unsigned int> (kVmRamSlots);
    while (offs < slots)
    {
        int valid = 0;
        EEL_F* p = NSEEL_VM_getramptr_noalloc (vm, offs, &valid);
        if (p == nullptr || valid <= 0)
        {
            offs = (offs / NSEEL_RAM_ITEMSPERBLOCK + 1) * NSEEL_RAM_ITEMSPERBLOCK;
            continue;
        }
        unsigned int n = static_cast<unsigned int> (valid);
        if (n > slots - offs)
            n = slots - offs;
        std::memset (p, 0, n * sizeof (EEL_F));
        offs += n;
    }
}

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
        compileError_ = "EEL2 JIT unavailable";
        return;
    }

    vm_ = NSEEL_VM_alloc();
    if (vm_ == nullptr)
    {
        compileError_ = "NSEEL_VM_alloc failed";
        return;
    }

    setupVm (vm_, &gram_, &rackCtx_, kVmRamSlots);

    bool ok = true;
    freq_ = mustRegVar (vm_, "freq", ok);
    note_ = mustRegVar (vm_, "note", ok);
    vel_ = mustRegVar (vm_, "vel", ok);
    gate_ = mustRegVar (vm_, "gate", ok);
    trig_ = mustRegVar (vm_, "trig", ok);
    srate_ = mustRegVar (vm_, "srate", ok);
    bpm_ = mustRegVar (vm_, "bpm", ok);
    beat_ = mustRegVar (vm_, "beat", ok);
    for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
        macro_[i] = mustRegVar (vm_, kMacroNames[i], ok);
    mw_ = mustRegVar (vm_, "mw", ok);
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
    *bpm_ = Transport::kDefaultBpm;
    for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
        *macro_[i] = 0.5;

    // Init is optional (blank = no init code); voice must have statements.
    bool failed = false;
    if (! isBlankCode (patch.init))
    {
        initCode_ = NSEEL_code_compile (vm_, patch.init, 0);
        if (initCode_ == nullptr)
        {
            compileSection_ = "init";
            compileError_ = codeError (vm_);
            failed = true;
        }
    }
    if (! failed)
    {
        if (isBlankCode (patch.voice))
        {
            compileSection_ = "voice";
            compileError_ = "voice code is empty";
            failed = true;
        }
        else
        {
            voiceCode_ = NSEEL_code_compile (vm_, patch.voice, 0);
            if (voiceCode_ == nullptr)
            {
                compileSection_ = "voice";
                compileError_ = codeError (vm_);
                failed = true;
            }
        }
    }
    if (failed)
    {
        if (initCode_ != nullptr) { NSEEL_code_free (initCode_); initCode_ = nullptr; }
        if (voiceCode_ != nullptr) { NSEEL_code_free (voiceCode_); voiceCode_ = nullptr; }
        NSEEL_VM_free (vm_);
        vm_ = nullptr;
        freq_ = note_ = vel_ = gate_ = trig_ = srate_ = bpm_ = beat_ = mw_ = out_ = nullptr;
        for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
            macro_[i] = nullptr;
        return;
    }
    collectVars (vm_, vars_);
    // M10 macro maps read the VM's own m1..m8 (what the code reads).
    static_assert (sizeof (EEL_F) == sizeof (double), "EEL_F must be double");
    for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
        rackStorage_.macroVars[i] = macro_[i];
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
void EelVoice::noteOn (double freq, double note, double vel,
                       const Transport& transport, double mw) noexcept
{
    if (vm_ == nullptr || freq_ == nullptr)
        return;
    *freq_ = freq;
    if (mw_ != nullptr) *mw_ = mw;
    if (bpm_ != nullptr) *bpm_ = transport.bpm;
    if (beat_ != nullptr) *beat_ = transport.beat;
    if (note_ != nullptr) *note_ = note;
    if (vel_ != nullptr) *vel_ = vel;
    if (gate_ != nullptr) *gate_ = 1.0;
    if (trig_ != nullptr) *trig_ = 1.0;
    if (initCode_ != nullptr)
    {
        rackStorage_.beginSample();
        NSEEL_code_execute (initCode_);
    }
    if (trig_ != nullptr) *trig_ = 1.0; // even if init wrote it
    if (gate_ != nullptr) *gate_ = 1.0;
}

void EelVoice::resetState() noexcept
{
    rackStorage_.reset();
    if (vm_ == nullptr)
        return;
    for (EEL_F_PTR v : vars_)
        *v = 0.0;
    zeroVmRam (vm_);
    if (srate_ != nullptr)
        *srate_ = rackStorage_.sampleRate;
    if (bpm_ != nullptr)
        *bpm_ = Transport::kDefaultBpm;
}

const EEL_F* EelVoice::variable (const char* name) noexcept
{
    return vm_ != nullptr ? NSEEL_VM_regvar (vm_, name) : nullptr;
}

float EelVoice::renderSample (double freq, double gate, double note, double vel,
                              const double macros[MacroSmoother::kNumMacros],
                              const Transport& transport, double mw) noexcept
{
    if (voiceCode_ == nullptr || freq_ == nullptr || out_ == nullptr)
        return 0.0f;
    *freq_ = freq;
    if (mw_ != nullptr) *mw_ = mw;
    if (gate_ != nullptr) *gate_ = gate;
    if (note_ != nullptr) *note_ = note;
    if (vel_ != nullptr) *vel_ = vel;
    if (srate_ != nullptr) *srate_ = rackStorage_.sampleRate;
    if (bpm_ != nullptr) *bpm_ = transport.bpm;
    if (beat_ != nullptr) *beat_ = transport.beat;
    for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
        if (macro_[i] != nullptr)
            *macro_[i] = macros[i];
    rackStorage_.beginSample(); // stateless shapers meter by call index (M9)
    NSEEL_code_execute (voiceCode_);
    if (trig_ != nullptr) *trig_ = 0.0;
    return rawOutput (*out_);
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
    voiceRackStorage_.init (sampleRate > 0.0 ? sampleRate : 48000.0, kRackSeed);
    voiceRackStorage_.rackKind = 1;  // M10: the master's nodes (RackKind::Master)
    rackCtx_.voice = &voiceRackStorage_;
    rackCtx_.master = &rackStorage_;

    if (! ensureEelInitialised() || ! isJitUsable())
    {
        compileError_ = "EEL2 JIT unavailable";
        return;
    }

    vm_ = NSEEL_VM_alloc();
    if (vm_ == nullptr)
    {
        compileError_ = "NSEEL_VM_alloc failed (master)";
        return;
    }

    setupVm (vm_, &gram_, &rackCtx_, kVmRamSlots);

    bool ok = true;
    inl_ = mustRegVar (vm_, "inl", ok);
    inr_ = mustRegVar (vm_, "inr", ok);
    outl_ = mustRegVar (vm_, "outl", ok);
    outr_ = mustRegVar (vm_, "outr", ok);
    srate_ = mustRegVar (vm_, "srate", ok);
    bpm_ = mustRegVar (vm_, "bpm", ok);
    beat_ = mustRegVar (vm_, "beat", ok);
    for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
        macro_[i] = mustRegVar (vm_, kMacroNames[i], ok);
    mw_ = mustRegVar (vm_, "mw", ok);
    if (! ok)
    {
        compileError_ = "NSEEL_VM_regvar failed (master)";
        NSEEL_VM_free (vm_);
        vm_ = nullptr;
        return;
    }
    *srate_ = rackStorage_.sampleRate;
    *bpm_ = Transport::kDefaultBpm;
    for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
        *macro_[i] = 0.5;

    // Blank master = passthrough (SPEC §4.3 default); the VM stays
    // allocated but nothing is compiled or executed.
    if (isBlankCode (patch.master))
    {
        passthrough_ = true;
        return;
    }
    masterCode_ = NSEEL_code_compile (vm_, patch.master, 0);
    if (masterCode_ == nullptr)
    {
        compileError_ = codeError (vm_);
        NSEEL_VM_free (vm_);
        vm_ = nullptr;
        inl_ = inr_ = outl_ = outr_ = srate_ = bpm_ = beat_ = mw_ = nullptr;
        for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
            macro_[i] = nullptr;
        return;
    }
    collectVars (vm_, vars_);
    for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
        voiceRackStorage_.macroVars[i] = macro_[i];
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
                              const double macros[MacroSmoother::kNumMacros],
                              const Transport& transport, double mw) noexcept
{
    if (masterCode_ == nullptr || inl_ == nullptr || outl_ == nullptr)
    {
        outL = inL;
        outR = inR;
        return;
    }
    *inl_ = inL;
    if (inr_ != nullptr) *inr_ = inR;
    if (srate_ != nullptr) *srate_ = rackStorage_.sampleRate;
    if (bpm_ != nullptr) *bpm_ = transport.bpm;
    if (beat_ != nullptr) *beat_ = transport.beat;
    if (mw_ != nullptr) *mw_ = mw;
    for (int i = 0; i < MacroSmoother::kNumMacros; ++i)
        if (macro_[i] != nullptr)
            *macro_[i] = macros[i];
    if (outl_ != nullptr) *outl_ = inL;
    if (outr_ != nullptr) *outr_ = inR;
    voiceRackStorage_.beginSample();
    NSEEL_code_execute (masterCode_);
    outL = rawOutput (*outl_);
    outR = (outr_ != nullptr) ? rawOutput (*outr_) : outL;
}

const EEL_F* EelMaster::variable (const char* name) noexcept
{
    return vm_ != nullptr ? NSEEL_VM_regvar (vm_, name) : nullptr;
}

void EelMaster::resetRacks() noexcept
{
    rackStorage_.reset();
    voiceRackStorage_.reset();
}

void EelMaster::resetState() noexcept
{
    resetRacks();
    if (vm_ == nullptr)
        return;
    for (EEL_F_PTR v : vars_)
        *v = 0.0;
    zeroVmRam (vm_);
    if (srate_ != nullptr)
        *srate_ = rackStorage_.sampleRate;
    if (bpm_ != nullptr)
        *bpm_ = Transport::kDefaultBpm;
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
