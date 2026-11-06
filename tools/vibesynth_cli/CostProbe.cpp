// Developer tool: vibesynth_cli --cost-probe (phase 2 study of whole-process
// cost jumps). Measures one patch in this process and prints one line of
// key=value pairs: the voice and master cost as the selftest measures them,
// the "out = 0" baseline before and after as a machine-state control, and
// where the hot state lives (rack, module states, VM variables, VM RAM, JIT
// code and work table, stack), so many fresh processes can be compared.
//
//   --cost-probe <built-in | file.pulsar | voice:CODE>
//       [--player]          PatchPlayer path: 8 notes held, then each of the
//                           program's 10 voices timed on its own
//       [--heap-shift N]    bench path: place the voice object at N bytes past
//                           a 4096-byte boundary (N a multiple of 64)
//       [--stack-shift N]   move the stack down by N bytes before measuring
//       [--ftz]             flush denormals (FTZ and DAZ) while measuring
//       [--runs N] [--samples N]   default 3 x 24000, as the selftest
//       [--repeats N]       measure again N-1 times later in the process
//       [--replace FROM TO]  edit the voice code first (repeatable), to find
//                           which calls a slow placement slows down
//
// Not used by any test; users never need it.

#include "Bench.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#if defined(_MSC_VER)
 #include <malloc.h>
 #define PROBE_ALLOCA _alloca
#else
 #include <alloca.h>
 #define PROBE_ALLOCA alloca
#endif

#if defined(__x86_64__) || defined(_M_X64)
 #include <xmmintrin.h>
 #define PROBE_HAS_MXCSR 1
#else
 #define PROBE_HAS_MXCSR 0
#endif

#if defined(__linux__)
 #include <sched.h>
#endif

#include <juce_core/juce_core.h>

#include "engine/NodeOverrides.h"
#include "patch/PatchPlayer.h"
#include "patch/PatchTweaks.h"
#include "patch/PatchValidator.h"

namespace vibesynth_cli {
namespace {

constexpr double kHostRate = 48000.0;

// The first fields of EEL2's codeHandleType (ns-eel-int.h), read-only.
struct CodeHandleHead
{
    void* blocksCode;
    void* blocksData;
    void* workTable;
    void* code;
};

struct Options
{
    std::string patchArg;
    bool player = false;
    long heapShift = -1;
    long stackShift = 0;
    bool ftz = false;
    int runs = 3;
    int samples = 24000;
    int repeats = 1;  // extra rounds later in the same process (is a slow reading persistent?)
    std::vector<std::pair<std::string, std::string>> replacements;  // voice code edits
};

unsigned long long addr (const void* p)
{
    return static_cast<unsigned long long> (reinterpret_cast<uintptr_t> (p));
}

// Clock control with no memory traffic: ns per step of one dependent
// multiply-xorshift chain (a fixed number of cycles per step), median of 5.
// It scales only with the core clock (and with a busy SMT sibling).
double aluNs()
{
    std::vector<double> t;
    volatile uint64_t seed = 1;
    volatile uint64_t sink = 0;
    for (int r = 0; r < 5; ++r)
    {
        uint64_t x = seed;
        constexpr int kSteps = 2000000;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kSteps; ++i)
            x = (x ^ (x >> 13)) * 0x9E3779B97F4A7C15ull;
        const auto t1 = std::chrono::steady_clock::now();
        sink = sink + x;
        t.push_back (std::chrono::duration<double, std::nano> (t1 - t0).count() / kSteps);
    }
    std::sort (t.begin(), t.end());
    return t[t.size() / 2];
}

void applyReplacements (std::string& code, const Options& o)
{
    for (const auto& r : o.replacements)
    {
        size_t at = 0;
        while ((at = code.find (r.first, at)) != std::string::npos)
        {
            code.replace (at, r.first.size(), r.second);
            at += r.second.size();
        }
    }
}

int currentCpu()
{
#if defined(__linux__)
    return sched_getcpu();
#else
    return -1;
#endif
}

unsigned int mxcsr()
{
#if PROBE_HAS_MXCSR
    return _mm_getcsr();
#else
    return 0u;
#endif
}

int findBuiltinIndex (const std::string& arg)
{
    const juce::String a (arg);
    const juce::String want = a.endsWithIgnoreCase (".pulsar") ? a.dropLastCharacters (7) : a;
    for (int i = 0; i < vibesynth::numBuiltinPresets(); ++i)
    {
        const juce::String stem = juce::String (vibesynth::builtinPresetFile (i)).upToLastOccurrenceOf (".", false, false);
        if (want.equalsIgnoreCase (stem) || want.equalsIgnoreCase (juce::String (vibesynth::builtinPresetName (i))))
            return i;
    }
    return -1;
}

bool loadPatch (const std::string& arg, vibesynth::Patch& out, std::string& label)
{
    using namespace vibesynth;
    if (arg.rfind ("voice:", 0) == 0)
    {
        out = Patch {};
        out.name = "probe";
        out.voiceMode = "poly";
        out.voice = arg.substr (6);
        out.master = "outl = inl;\noutr = inr;";
        label = "voice-code";
        return true;
    }
    std::string text;
    const juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (arg));
    if (f.existsAsFile())
    {
        text = f.loadFileAsString().toStdString();
        label = f.getFileNameWithoutExtension().toStdString();
    }
    else
    {
        const int index = findBuiltinIndex (arg);
        if (index < 0)
            return false;
        text = builtinPresetText (index);
        label = juce::String (builtinPresetFile (index)).upToLastOccurrenceOf (".", false, false).toStdString();
    }
    const ParseResult pr = parsePatch (text);
    if (! pr.ok())
        return false;
    out = pr.patch;
    return true;
}

void macroDefaults (const vibesynth::Patch& p, double macros[vibesynth::MacroSmoother::kNumMacros])
{
    for (int m = 0; m < vibesynth::MacroSmoother::kNumMacros; ++m)
        macros[m] = p.macroDefault (m);
}

// The "out = 0" voice, timed like the patch (machine-state control).
double baselineNs (const Options& o, double voiceRate)
{
    const vibesynth::PatchStrings ps = { "0;", "out = 0;", "outl = inl; outr = inr;" };
    const auto v = std::make_unique<vibesynth::EelVoice> (ps, voiceRate, 7u);
    double macros[vibesynth::MacroSmoother::kNumMacros] = {};
    return v->isValid() ? benchTimeVoice (*v, 220.0, 57.0, macros, voiceRate, o.runs, o.samples) : -1.0;
}

// Where one voice's hot state lives.
std::string voiceLayout (vibesynth::EelVoice& v, const char* prefix)
{
    using namespace vibesynth;
    VoiceRack& r = v.rack();
    const auto* h = static_cast<const CodeHandleHead*> (v.voiceCodeHandle());
    int valid = 0;
    const EEL_F* ram0 = v.rackContext().vm != nullptr
        ? NSEEL_VM_getramptr_noalloc (static_cast<NSEEL_VMCTX> (v.rackContext().vm), 0, &valid) : nullptr;
    const EEL_F* freqVar = v.variable ("freq");
    char buf[1024];
    std::snprintf (buf, sizeof (buf),
                   "%sobj=%llx %srack=%llx %soscwt=%llx %sformant=%llx %sfltlp=%llx %snotch=%llx "
                   "%soscsine=%llx %soscsaw=%llx %sadsr=%llx %slfo=%llx %sadaa=%llx %smeters=%llx "
                   "%scode=%llx %swork=%llx %svars=%llx %sram=%llx",
                   prefix, addr (&v), prefix, addr (&r), prefix, addr (&r.oscWt[0]), prefix, addr (&r.fltFormant[0]),
                   prefix, addr (&r.fltLp[0]), prefix, addr (&r.fltNotch[0]), prefix, addr (&r.oscSine[0]),
                   prefix, addr (&r.oscSaw[0]), prefix, addr (&r.envAdsr[0]), prefix, addr (&r.modLfo[0]),
                   prefix, addr (&r.adaa[0][0]), prefix, addr (&r.meters[0][0]),
                   prefix, addr (h != nullptr ? h->code : nullptr), prefix, addr (h != nullptr ? h->workTable : nullptr),
                   prefix, addr (freqVar), prefix, addr (ram0));
    return buf;
}

// noinline: its frame sits below the caller's alloca, so --stack-shift moves it.
#if defined(_MSC_VER)
 #define PROBE_NOINLINE __declspec(noinline)
#else
 #define PROBE_NOINLINE __attribute__ ((noinline))
#endif

PROBE_NOINLINE int probeBench (const Options& o, const vibesynth::Patch& patch, const std::string& label,
                               int oversampling)
{
    using namespace vibesynth;
    const double voiceRate = kHostRate * oversampling;
    int stackMarker = 0;
    const int cpu0 = currentCpu();
    warmUp();
    const double alu1 = aluNs();
    const double base1 = baselineNs (o, voiceRate);

    const PatchStrings ps = { patch.init.c_str(), patch.voice.c_str(), patch.master.c_str() };
    double macros[MacroSmoother::kNumMacros] = {};
    macroDefaults (patch, macros);

    // The voice: make_unique as the selftest, or placed at a chosen page offset.
    std::unique_ptr<EelVoice> owned;
    std::unique_ptr<char[]> arena;
    EelVoice* v = nullptr;
    if (o.heapShift >= 0)
    {
        arena.reset (new char[sizeof (EelVoice) + 8192 + (size_t) o.heapShift]);
        const uintptr_t base = (reinterpret_cast<uintptr_t> (arena.get()) + 4095u) & ~uintptr_t { 4095u };
        v = new (reinterpret_cast<void*> (base + (uintptr_t) o.heapShift)) EelVoice (ps, voiceRate, 9u);
    }
    else
    {
        owned = std::make_unique<EelVoice> (ps, voiceRate, 9u);
        v = owned.get();
    }
    const auto m = std::make_unique<EelMaster> (ps, kHostRate);
    double voiceNs = -1.0, masterNs = -1.0;
    if (v->isValid() && m->isValid())
    {
        voiceNs = benchTimeVoice (*v, 55.0, 33.0, macros, voiceRate, o.runs, o.samples);
        const std::vector<float> in = benchRenderInput (*v, 55.0, macros, oversampling, o.samples);
        masterNs = benchTimeMaster (*m, in, macros, o.runs, o.samples);
    }
    const double base2 = baselineNs (o, voiceRate);
    const double alu2 = aluNs();
    // Later rounds: the same voice object again, each after its own baseline.
    std::string reps;
    for (int k = 1; k < o.repeats && voiceNs >= 0.0; ++k)
    {
        const double b = baselineNs (o, voiceRate);
        const double vn = benchTimeVoice (*v, 55.0, 33.0, macros, voiceRate, o.runs, o.samples);
        char t[64];
        std::snprintf (t, sizeof (t), "%s%.1f/%.1f", k == 1 ? " rep=" : ",", vn, b);
        reps += t;
    }
    std::printf ("probe path=bench patch=%s voice=%.1f master=%.1f base1=%.1f base2=%.1f alu1=%.3f alu2=%.3f "
                 "cpu0=%d cpu1=%d mxcsr=%x heapshift=%ld stackshift=%ld stack=%llx text=%llx %s%s\n",
                 label.c_str(), voiceNs, masterNs, base1, base2, alu1, alu2, cpu0, currentCpu(), mxcsr(),
                 o.heapShift, o.stackShift, addr (&stackMarker),
                 addr (reinterpret_cast<const void*> (&NSEEL_code_execute)),
                 voiceLayout (*v, "").c_str(), reps.c_str());
    if (o.heapShift >= 0)
        v->~EelVoice();
    return voiceNs >= 0.0 ? 0 : 1;
}

PROBE_NOINLINE int probePlayer (const Options& o, const vibesynth::Patch& patchIn, const std::string& label)
{
    using namespace vibesynth;
    int stackMarker = 0;
    const int cpu0 = currentCpu();
    warmUp();
    Patch p = patchIn;
    p.master = "outl = inl;\noutr = inr;";  // as the bench's PatchPlayer rows: voices only
    p.mods = PatchMods {};
    std::string error;
    const auto program = PatchProgram::build (p, kHostRate, error);
    if (program == nullptr)
    {
        std::printf ("probe path=player patch=%s BUILD FAIL: %s\n", label.c_str(), error.c_str());
        return 1;
    }
    const double voiceRate = program->voiceRate();
    const double alu1 = aluNs();
    const double base1 = baselineNs (o, voiceRate);

    constexpr int kChunk = 256;
    float targets[PatchPlayer::kNumMacros] = {};
    for (int mi = 0; mi < PatchPlayer::kNumMacros; ++mi)
        targets[mi] = static_cast<float> (p.macroDefault (mi));
    auto table = std::make_unique<OverrideTable>();
    applyPatchTweaks (p, *table);
    auto player = std::make_unique<PatchPlayer>();
    player->setProgram (program.get());
    player->prepare (kHostRate, kChunk, targets);
    player->setHostTransport (140.0, false, false, 0.0);
    player->setVoiceControls (PatchPlayer::VoiceMode::Patch, 0.0);
    player->setNodeHooks (table.get(), nullptr);

    struct Ev { int samplePosition; const uint8_t* data; int numBytes; };
    const int notes[8] = { 45, 52, 57, 60, 64, 67, 69, 72 };
    std::array<std::array<uint8_t, 3>, 8> on {};
    std::vector<Ev> evs;
    for (int k = 0; k < 8; ++k)
    {
        on[(size_t) k] = { 0x90, static_cast<uint8_t> (notes[k]), 100 };
        evs.push_back ({ 0, on[(size_t) k].data(), 3 });
    }
    std::vector<Ev> none;
    volatile float sink = 0.0f;
    int pos = 0;
    // One run = o.samples host samples with every note held; run -1 is untimed.
    std::vector<double> times;
    for (int r = -1; r < 9; ++r)
    {
        double voiceSamples = 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int done = 0; done < o.samples; done += kChunk)
        {
            auto& list = (pos == 0) ? evs : none;
            auto it = list.cbegin();
            player->renderChunk (kChunk, targets, it, list.cend(), pos);
            sink = sink + player->outL()[0];
            voiceSamples += static_cast<double> (player->soundingVoices()) * kChunk * program->oversampling();
            pos += kChunk;
        }
        const auto t1 = std::chrono::steady_clock::now();
        if (r >= 0)
            times.push_back (std::chrono::duration<double, std::nano> (t1 - t0).count() / std::max (1.0, voiceSamples));
    }
    std::sort (times.begin(), times.end());
    const double playerNs = times[times.size() / 2];
    const int sounding = player->soundingVoices();

    // Each of the program's voices on its own (the same objects the player used).
    double macros[MacroSmoother::kNumMacros] = {};
    macroDefaults (p, macros);
    std::string perVoice, layout;
    for (int i = 0; i < PatchProgram::kNumVoices; ++i)
    {
        const double ns = benchTimeVoice (program->voice (i), 55.0, 33.0, macros, voiceRate, o.runs, o.samples);
        char b[32];
        std::snprintf (b, sizeof (b), " v%d=%.1f", i, ns);
        perVoice += b;
        char pre[8];
        std::snprintf (pre, sizeof (pre), "v%d", i);
        layout += " " + voiceLayout (program->voice (i), pre);
    }
    // Later rounds over all 10 voices (round-robin), appended as v<i>r=a,b,...
    std::vector<std::string> later (PatchProgram::kNumVoices);
    for (int k = 1; k < o.repeats; ++k)
        for (int i = 0; i < PatchProgram::kNumVoices; ++i)
        {
            const double ns = benchTimeVoice (program->voice (i), 55.0, 33.0, macros, voiceRate, o.runs, o.samples);
            char b[32];
            std::snprintf (b, sizeof (b), "%s%.1f", k == 1 ? "" : ",", ns);
            later[(size_t) i] += b;
        }
    if (o.repeats > 1)
        for (int i = 0; i < PatchProgram::kNumVoices; ++i)
            perVoice += " v" + std::to_string (i) + "r=" + later[(size_t) i];
    const double base2 = baselineNs (o, voiceRate);
    const double alu2 = aluNs();
    std::printf ("probe path=player patch=%s player=%.1f sounding=%d base1=%.1f base2=%.1f alu1=%.3f alu2=%.3f "
                 "cpu0=%d cpu1=%d mxcsr=%x stackshift=%ld stack=%llx text=%llx%s%s\n",
                 label.c_str(), playerNs, sounding, base1, base2, alu1, alu2, cpu0, currentCpu(), mxcsr(),
                 o.stackShift, addr (&stackMarker),
                 addr (reinterpret_cast<const void*> (&NSEEL_code_execute)), perVoice.c_str(), layout.c_str());
    return 0;
}

} // namespace

int runCostProbe (int argc, char** argv, int oversampling)
{
    Options o;
    if (argc < 1)
        return 2;
    o.patchArg = argv[0];
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (a == "--player")
            o.player = true;
        else if (a == "--ftz")
            o.ftz = true;
        else if (a == "--heap-shift" && hasValue)
            o.heapShift = std::strtol (argv[++i], nullptr, 10);
        else if (a == "--stack-shift" && hasValue)
            o.stackShift = std::strtol (argv[++i], nullptr, 10);
        else if (a == "--replace" && i + 2 < argc)
        {
            o.replacements.emplace_back (argv[i + 1], argv[i + 2]);
            i += 2;
        }
        else if (a == "--repeats" && hasValue)
            o.repeats = std::max (1, std::min (50, std::atoi (argv[++i])));
        else if (a == "--runs" && hasValue)
            o.runs = std::max (1, std::atoi (argv[++i]));
        else if (a == "--samples" && hasValue)
            o.samples = std::max (256, std::atoi (argv[++i]));
        else
        {
            std::printf ("bad --cost-probe argument: %s\n", a.c_str());
            return 2;
        }
    }
    if (o.heapShift >= 0 && (o.heapShift % 64 != 0 || o.heapShift >= 65536))
    {
        std::printf ("--heap-shift must be a multiple of 64 below 65536\n");
        return 2;
    }
    if (o.stackShift < 0 || o.stackShift > 1 << 20)
    {
        std::printf ("--stack-shift must be 0..1048576\n");
        return 2;
    }
    vibesynth::Patch patch;
    std::string label;
    if (! loadPatch (o.patchArg, patch, label))
    {
        std::printf ("no file or built-in preset named \"%s\" (or it doesn't parse)\n", o.patchArg.c_str());
        return 2;
    }
    applyReplacements (patch.voice, o);
#if PROBE_HAS_MXCSR
    if (o.ftz)
        _mm_setcsr (_mm_getcsr() | 0x8040u);
#endif
    char* pad = static_cast<char*> (PROBE_ALLOCA ((size_t) o.stackShift + 16));
    pad[0] = 0;
    volatile char keep = pad[0];
    (void) keep;
    return o.player ? probePlayer (o, patch, label) : probeBench (o, patch, label, oversampling);
}

} // namespace vibesynth_cli
