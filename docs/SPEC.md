# VibeSynthVST — Specification (v2)

> v2 replaces the original architecture draft. Key changes: EEL2 replaces
> libtcc, the DSP library list is cut to an MVP core, the safety system gains a
> runtime guard, the node visualizer becomes read-only and code-derived, and the
> scope is cut to what one developer can ship. Sections marked **[MVP]** are in
> scope for the hackathon; **[LATER]** items must not be built until all MVP
> milestones pass.

---

## 1. Background & constraints

- **Developer:** electronic bass music producer (140 BPM minimal dubstep, UKG,
  neurofunk, sub-bass design), self-taught engineer. Previously built Pulse DAW
  (Android: Kotlin, Compose, C++17/NDK, Oboe, lock-free SPSC ring buffers).
- **Dev machine:** Valve Steam Deck, SteamOS Desktop Mode. No Windows or Mac
  hardware. All Windows/macOS verification happens in GitHub Actions.
- **Toolchain:** VS Code-based IDE + Cline (plan: `muse-wonder-1.0`, act:
  `muse-spark-1.3`). Builds run inside an Ubuntu 22.04 Distrobox named `juce`
  (matches the Linux CI runner). CMake + Ninja. REAPER (Flatpak) for testing.
- **Target formats:** VST3 + Standalone on Windows x64, macOS (x86_64 + arm64),
  Linux x86_64.

---

## 2. Core idea & pitch

The user types a description of a sound ("weeping neuro reese with a throat that
closes on the mod wheel"). An LLM writes a small per-sample DSP program plus 8
macro knob labels. The plugin JIT-compiles it to native code, safety-tests it
offline, and hot-swaps it into the running synth with no clicks or dropouts.

**What is actually novel (pitch this):** natural-language prompt → AI-written
sample-rate code → validated and hot-swapped live while playing, plus
audition-and-keep mutation.

**Claims we must NOT make:**
- That commercial or modular tools can't do audio-rate modulation. VCV Rack,
  Bitwig Grid, Reaktor Core and Max gen~ already do single-sample modular.
- "Zero-delay feedback". Our feedback is **one-sample feedback** (value written
  on sample n is read on sample n+1). ZDF means something else in DSP.
- "Genetic algorithm". Mutation is LLM-guided variation with the user as the
  selector.

---

## 3. Architecture overview

```
 Message thread (UI)           Generation worker (juce::Thread)          Audio thread
 ───────────────────           ───────────────────────────────          ────────────
 Prompt / Mutate ──request──▶  1. Build prompt (full API + recipes)
                               2. HTTPS → Meta API (juce::URL)
                               3. Parse response (JSON header + code)
                               4. Compile EEL2 into 9 fresh VMs
                                  (8 voices + 1 master)
                               5. Offline test matrix (§7.1)
                                  fail → feed error back, retry ≤2
                               6. Push PatchProgram* ──SPSC queue──▶   fade out old (5 ms)
                                                                       swap, reset racks
                                                                       fade in new (5 ms)
                               7. Free old program  ◀──SPSC queue───  "done with old"
 Labels/graph update ◀───────  (message thread notified)
```

Rules:
- The audio thread never allocates, locks, logs, throws or does I/O.
- Ownership of a `PatchProgram` moves between threads only through SPSC queues.
  The audio thread never frees a program. It returns retired programs on a queue
  and the worker frees them.
- One generation request runs at a time (Mutate's 4 variants run in sequence or
  in parallel on the worker, never on the audio thread).

---

## 4. Patch language: EEL2 (replaces libtcc)

### 4.1 Why EEL2 and not libtcc
libtcc runs AI-written C natively in the DAW process with full user privileges:
access to libc (`system`, `fopen`), raw pointers, integer divide-by-zero and
infinite loops can crash the DAW or damage the user's machine, and segfaults
can't be caught in-process. libtcc is also LGPL, and macOS (arm64, MAP_JIT,
Universal Binaries, libtcc1 runtime helpers) is fragile.

**EEL2** (from Cockos WDL, zlib-style licence; the language behind REAPER JSFX):
- JIT-compiles to native code in ~1 ms on x86-64 and arm64.
- Sandboxed: no pointers, no syscalls, bounds-checked virtual memory, float-only
  maths (divide by zero gives inf, not a crash), loop iteration caps.
- Native C++ functions can be registered, and an opaque context pointer
  (`NSEEL_VM_SetCustomFuncThis`) lets each function find its VoiceRack.
- LLMs have seen large amounts of JSFX/EEL2 code.
- Reference integration: **ysfx** (open-source JSFX host that builds EEL2 with
  CMake on all three OSes).
- Fallback: if JIT fails on a platform, EEL2's portable (non-JIT) build mode.
  Verify JIT on all three CI targets in Milestone 3.

### 4.2 Execution model
- Each patch has three code sections: `init` (optional, runs on every note-on for
  that voice), `voice` (runs once per sample per active voice), `master` (runs
  once per sample on the stereo bus).
- Each of the 8 voices gets its own EEL2 VM, compiled from the same source, so
  user variables are per-voice state. The master has one VM.
- The C++ voice loop runs per sample: write input variables → execute `voice`
  code → read `out`. MIDI events are therefore sample-accurate with no block
  splitting, and `trig` is exactly 1 on the note-on sample.
- **[MVP, Milestone 7]** Voice section runs at 2× the host sample rate with
  half-band decimation (anti-aliasing for folding, feedback FM and chaos).
  Master runs at 1×.
- Any user-created variable persists between samples (that's how state works).
  A newly loaded patch starts with all variables at 0.
- VM memory (`mem[]`, for user delay lines or tables) is capped per VM
  (e.g. 65,536 slots).

### 4.3 Variables bound into each VM

| Voice VM | Meaning |
|---|---|
| `freq` | note frequency in Hz (includes glide and pitch bend) |
| `note` | MIDI note number |
| `vel` | velocity 0–1 |
| `gate` | 1 while the key is held, 0 after release |
| `trig` | 1 on the note-on sample only |
| `srate` | the voice's running sample rate (2× host when oversampling) |
| `m1`…`m8` | macro values 0–1, smoothed per sample in C++ (~10 ms one-pole) |
| `out` | **write**: the voice's output sample |
| `fb1`, `fb2`, `prev` | conventions for one-sample feedback and zero-crossing detection (plain variables) |

| Master VM | Meaning |
|---|---|
| `inl`, `inr` | summed voice output |
| `outl`, `outr` | **write**: bus output (default passthrough) |
| `m1`…`m8`, `srate` | as above |

---

## 5. DSP lexicon (native functions exposed to EEL2)

### 5.1 Design rules (non-negotiable)
1. **Normalized inputs.** Every "amount" parameter is 0–1 and is mapped to a
   musical range inside the wrapper. Frequency parameters are in Hz.
2. **Clamp everything.** Inputs outside range are clamped. Frequencies are
   clamped to [0, 0.45 × srate]. Resonance can't reach self-destruction. Slot
   numbers out of range clamp to 0.
3. **Slots.** Stateful functions take `slot` as their first argument (a literal
   integer). Each slot is a pre-allocated module instance in the voice's
   `VoiceRack`. Using slot 0 for two different calls of the same function shares
   state; the system prompt tells the model to use distinct slots.
4. **Auto-metering.** Every stateful call records |output| peak into
   `rack->meters[module][slot]` and sets a "touched" bit. The UI reads these
   (§9) and the validator uses the touched set to build the real node list.
5. **Physical models take an exciter.** `phys_*` functions take `exciter` as a
   per-sample input so any signal can drive them.
6. All module state is allocated in `prepareToPlay`. Wrappers never allocate.
7. Unused modules cost no CPU. On patch swap, all racks are reset.

### 5.2 Function list

**[MVP] core (DaisySP + our own code):**

| Function | Source | Notes |
|---|---|---|
| `osc_sine(slot, hz)` `osc_saw(slot, hz)` `osc_tri(slot, hz)` | DaisySP Oscillator (PolyBLEP) | 4 slots |
| `osc_square(slot, hz, pw)` | DaisySP | pw 0–1 |
| `osc_supersaw(slot, hz, detune, mix)` | own (7 PolyBLEP saws) or DaisySP | 2 slots |
| `noise()` | own | stateless white noise |
| `flt_lp(slot, in, cutoff, res)` `flt_hp(...)` `flt_bp(...)` | DaisySP Svf | cutoff 0–1 mapped exponentially 20 Hz–20 kHz; 3 slots |
| `flt_ladder(slot, in, cutoff, res)` | DaisySP MoogLadder (check it's not in DaisySP-LGPL; else own) | 2 slots |
| `env_adsr(slot, gate, a, d, s, r)` | DaisySP Adsr | times mapped 1 ms–10 s; 3 slots |
| `env_follow(slot, in)` | own | 2 slots |
| `fx_fold(in, amount)` | own (sin-fold) | stateless |
| `fx_sat(in, drive)` | own (tanh with gain compensation) | stateless |
| `fx_crush(slot, in, bits, rate)` | DaisySP Decimator | 2 slots |
| `fx_comb(slot, in, hz, feedback)` | own fractional delay | 2 slots |
| `phys_resonator(slot, exciter, hz, structure, brightness, damping)` | DaisySP Resonator | 1 slot |
| `phys_string(slot, exciter, hz, brightness, damping)` | DaisySP KarplusString | 1 slot |
| `chaos_lorenz(slot, rate, axis)` | own ChaosMath.h | rate 0–1 maps LFO range → audio range; axis 0/1/2 |
| `chaos_duffing(slot, hz, chaos)` | own ChaosMath.h | 2 slots |
| `util_safe_fb(slot, x)` | own | 20 Hz DC block + slew limit + tanh. All feedback must pass through this |
| `util_xover_lo(slot, in)` / `util_xover_hi(slot)` | own Linkwitz-Riley 4th order @ 100 Hz | `_hi` returns the high band of the last `_lo` call |
| `util_bytebeat(t, mode)` | own | bitwise helpers; integer ops done in C++ |

**[MVP] master bus functions:**

| Function | Notes |
|---|---|
| `bus_sat(in, drive)` | stereo-safe saturation (call per channel) |
| `bus_mono_low(l, r)` / `bus_mono_low_r()` | sums sub-100 Hz to mono, returns new L (and R via second call) |
| `bus_chorus(l, r, depth, mix)` | own or DaisySP Chorus |
| `bus_reverb(l, r, size, mix)` | own small FDN or DaisySP (non-LGPL only) |

Always on after the master code (not callable): DC block → peak limiter →
hard clip at ±1.0.

**[LATER]** (add one at a time, only after Milestone 10):
- Signalsmith DSP: Kaiser-sinc fractional delay, oversampling helpers (may be
  pulled in earlier for §4.2 oversampling), Hilbert IIR frequency shifter.
- Mutable Instruments Plaits engines via a per-voice block-buffer adapter
  (they render in 12–24 sample blocks; document the latency this adds).
- STK BandedWG / BlowHole / Shakers (need forking to accept an exciter input).
- Pink Trombone / voclib vocal tract (needs an external glottal input).
- Airwindows (ToTape, Spiral, DeRez) as master bus colour.

Removed entirely: Cycfi Q (we already know each voice's pitch; smoothers and
followers are a few lines of our own code), Mutable Elements (32 kHz internal,
block-based, overlaps DaisySP Resonator), Airwindows SubTight as a crossover
(replaced by `util_xover_*`).

---

## 6. LLM integration

### 6.1 Prompt construction
- The system prompt contains the **entire** function reference (§5.2) every
  time. No keyword RAG for the API (it causes the model to invent functions it
  wasn't shown).
- `recipes/*.md` hold technique examples (§8). Pick 2–4 recipes by keyword
  match against the user prompt and append them. Always include at least one.
- Include 2 complete example patches from the built-in presets as few-shot
  examples.
- Files live in `resources/prompts/` and are embedded with JUCE BinaryData.

### 6.2 Response format
Ask the model for exactly this layout. Don't put code inside JSON strings; the
escaping breaks.

````
```json
{
  "name": "Weeping Reese",
  "macros": [ {"label": "Detune", "default": 0.4}, ... 8 entries ... ],
  "graph": { "edges": [["osc_supersaw:0", "flt_lp:0"], ["flt_lp:0", "out"]] }
}
```
```eel init
```
```eel voice
...
```
```eel master
...
```
````

Parser rules: missing `init` and `master` sections are allowed (defaults are
empty and passthrough). Missing `voice` or invalid JSON is a parse error and
goes through the retry path.

### 6.3 Models & parameters

| Use | Model | Temperature |
|---|---|---|
| Generate | `muse-spark-1.3` | 0.8 |
| Mutate / Breed | `muse-spark-1.3` | 0.9 |
| Retry after compile/test failure | `muse-spark-1.3` | 0.15 |

Model IDs and endpoint (`https://api.meta.ai/v1/chat/completions`,
OpenAI-compatible) are configurable in settings, not hardcoded. Expect 3–8 s per
generation. The UI shows progress, never blocks.

### 6.4 Self-heal
On parse, compile or test failure, send the model its own output plus the exact
reason (e.g. `EEL2 compile error line 7: ...`, or
`NaN produced at note=40, m3=1.0 after 0.31 s`). Maximum 2 retries, then report
failure in the UI and keep the current patch playing.

### 6.5 API key handling
- User enters a key in a settings panel; stored with `juce::PropertiesFile` in
  the user's app-data folder. Env var `VIBESYNTH_API_KEY` overrides it for
  development.
- The key is never compiled into the binary or committed to the repo.
- With no key, Generate/Mutate are disabled with a clear message; built-in
  presets still work.

### 6.6 Networking
`juce::URL` on the worker thread (WinHTTP on Windows, NSURLSession on macOS,
libcurl on Linux via `JUCE_USE_CURL=1`). 30 s timeout. In-flight requests are
cancelled and the worker joined in the processor destructor, so closing the DAW
mid-request can't crash.

---

## 7. Safety system

### 7.1 Offline test (worker thread, before any swap)
Render each patch through fresh test racks for **1 second** at each of:
- notes {28, 40, 55} × macros {all 0, defaults, all 1} = 9 configurations,
  with a note-on at t=0 and note-off at t=0.7 s.

Reject (and retry via §6.4) if any configuration shows:
- NaN or Inf (check with bit-pattern tests; see §11 on fast-math)
- peak above +12 dBFS before the limiter
- DC offset above 0.1 averaged over the last 0.5 s
- all 9 configurations silent (peak below −80 dBFS): "patch makes no sound"
- CPU over budget: average voice render time above 10 % of real time per voice

Target: the whole matrix finishes in under 300 ms.

**Trim:** compute one gain so the worst-case peak lands at −3 dBFS. The trim may
only attenuate (gain ≤ 1.0), never boost.

### 7.2 Runtime guard (audio thread, always on)
- `juce::ScopedNoDenormals` in `processBlock` (feedback and resonators produce
  denormals).
- After each voice's block: if any sample is non-finite, zero that voice's
  output, reset its rack and VM variables, and mute it for 20 ms.
- Master chain always ends with DC block → peak limiter → hard clip ±1.0.
- Final output is checked for non-finite values and zeroed if found.

### 7.3 Hot-swap protocol
1. Worker pushes the new `PatchProgram*` on the swap queue.
2. Audio thread fades the output out over 5 ms.
3. At silence it swaps programs, resets all racks (no allocation; reset only),
   and pushes the old program on the retire queue.
4. Fades in over 5 ms. Held notes keep sounding with the new patch.
5. Worker frees retired programs.

---

## 8. Sound design techniques (taught via recipes)

All examples use EEL2 syntax. These go in `resources/prompts/recipes/`.

**One-sample feedback loops.** Output from a folder or resonator feeds back into
pitch, FM index or cutoff on the next sample, through `util_safe_fb`.
```
x = osc_sine(0, freq * (1 + fb1 * m1 * 4));
y = fx_fold(flt_lp(0, x, m2, m3), m4);
fb1 = util_safe_fb(0, y);
out = y * env_adsr(0, gate, 0.01, 0.3, 0.8, 0.2);
```

**Cross-adaptive exciter injection.** Drive a physical model with a synth
signal instead of a click.
```
ex = osc_supersaw(0, freq * 1.01, m1, 0.5) * 0.3;
out = phys_resonator(0, ex, freq, m2, m3, m4);
```

**Wave-surgery (zero-crossing splicing).** Switch engines on zero-crossings of a
reference oscillator for click-free splicing. Run every engine every sample so
their phases stay continuous.
```
ref = osc_sine(0, freq);
(prev <= 0 && ref > 0) ? flip = !flip;
prev = ref;
a = osc_saw(1, freq);
b = phys_string(0, a * 0.2, freq, m1, m2);
out = flip ? a : b;
```

**Piece-wise transfer functions & chaos modulation.** Custom shapers inline;
chaos at LFO rate (`rate` near 0) or audio rate (near 1).
```
c = chaos_lorenz(0, m5 * 0.2, 0);
x = osc_saw(0, freq * (1 + c * m6 * 0.05));
out = x > 0 ? sin(x * (1 + m1 * 8)) : floor(x * 4) / 4;
```

**Split-brain sub bass.** Clean, phase-stable sub below 100 Hz; surgery above.
```
src = osc_saw(0, freq);
lo = util_xover_lo(0, src);
hi = util_xover_hi(0);
hi = fx_fold(flt_bp(0, hi, m1, m2), m3);
out = lo * 0.9 + hi * m4;
```
In `master`: `outl = bus_mono_low(inl, inr); outr = bus_mono_low_r();` then
`bus_sat` both channels.

---

## 9. Plugin behaviour & UI

### 9.1 Voices **[MVP]**
- 8-voice polyphony, plus **mono/legato mode** with glide time (bass patches are
  mostly mono). Patch JSON may set a default mode; the user can override.
- Voice stealing: oldest voice, 3 ms fast release before reuse.
- Pitch bend ±2 semitones; mod wheel is readable as variable `mw` (0–1).

### 9.2 Macros **[MVP]**
- 8 host parameters registered at startup as "Macro 1"…"Macro 8". Never added
  or removed, so automation never breaks.
- The plugin UI shows the patch's labels. Host automation lanes will still show
  "Macro N" in most DAWs. Call `updateHostDisplay` anyway, but don't rely on it.
- Loading a new patch sets macro values to its defaults via
  `setValueNotifyingHost` on the message thread.

### 9.3 State persistence **[MVP]**
`getStateInformation` saves the full patch (all code sections, JSON header,
original prompt, trim) plus macro values and voice mode. Reopening a project
recompiles the saved code locally. **It never calls the LLM.**

### 9.4 Built-in presets **[MVP]**
15+ hand-tuned patches in `resources/presets/*.vibepatch`, covering reese,
neuro, UKG organ/stab, sub, physical-model plucks, chaos textures. They are:
the out-of-the-box demo, few-shot examples (§6.1) and regression tests.

`.vibepatch` format:
```json
{
  "version": 1,
  "name": "Weeping Reese",
  "prompt": "the original user prompt",
  "macros": [{"label": "Detune", "default": 0.4}],
  "voiceMode": "mono",
  "graph": {"edges": [["osc_supersaw:0", "flt_lp:0"]]},
  "code": {"init": "", "voice": "...", "master": "..."}
}
```
Loading a patch file runs the same compile + offline test as a generated patch.

### 9.5 Editor **[MVP]**
- Prompt box, **Generate** button, status/progress line, error display.
- 8 macro knobs with dynamic labels.
- Preset browser (built-ins + user-saved patches).
- Settings panel: API key, model IDs.
- Code viewer (read-only text) showing the active patch.

### 9.6 Node visualizer **[MVP, read-only]**
- **Nodes** come from the *real* code: the set of (module, slot) pairs touched
  during the offline test.
- **Edges** come from the LLM's `graph.edges`, filtered to nodes that really
  exist. They are labelled as descriptive.
- Each node pulses with its own `rack->meters` peak (read atomically at 30 fps).
- **[LATER]** editable graph. That requires making the graph the source of truth
  (the plugin generates code from the graph, and the LLM writes graph +
  custom expression nodes). Not possible with free-form code.

### 9.7 Mutate / Breed **[MVP stretch]**
- **Mutate:** sends the active patch back with "make 4 distinct variations". All 4
  go through compile + test. The user auditions each (one click = swap) and
  keeps one.
- **Breed:** select two saved patches, ask for 4 hybrids, same audition flow.

---

## 10. Build, CI & distribution

### 10.1 CMake **[MVP]**
- CMake ≥ 3.25, Ninja, CPM.cmake. JUCE 8 via CPM. `juce_add_plugin` with
  `FORMATS VST3 Standalone`.
- DaisySP via CPM (has its own CMake). WDL/EEL2 vendored in
  `third_party/WDL` (only the `eel2` subset and what it needs). Follow ysfx's
  CMake for the per-platform JIT pieces (Windows x64 needs a prebuilt object or
  an assembler step; verify in Milestone 3).
- `JUCE_WEB_BROWSER=0`, `JUCE_USE_CURL=1` on Linux only.
- Windows: `CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded$<$<CONFIG:Debug>:Debug>`
  (static runtime, no VCRedist needed).
- macOS: `CMAKE_OSX_ARCHITECTURES="x86_64;arm64"`. Verify EEL2's JIT for both
  architectures in the Universal build (Milestone 3). If Universal fails, ship
  separate arm64 and x86_64 zips.
- `ccache` enabled when available (Steam Deck compile times).
- Extra target `vibesynth_cli` (§10.3).

### 10.2 GitHub Actions (`.github/workflows/build.yml`) **[MVP]**
- Triggers: push, pull_request, workflow_dispatch. Releases only on `v*` tags.
- Matrix: `ubuntu-22.04`, `windows-latest` (MSVC 2022 x64), `macos-14`.
- Steps: configure + build (Release) → run `vibesynth_cli --selftest` → pluginval
  (strictness 5) on the VST3 → zip VST3 + Standalone → `actions/upload-artifact`.
  On tags, `softprops/action-gh-release` publishes the three zips.
- Linux apt packages: ALSA, FreeType, X11 (Xrandr, Xinerama, Xcursor), libcurl.
  No WebKit.
- macOS: ad-hoc sign (`codesign --force --deep -s -`). This lets the code run
  on Apple Silicon but **does not bypass Gatekeeper**. The README must tell
  judges to run `xattr -cr <path to plugin/app>` or use System Settings →
  Privacy & Security → "Open Anyway".
- pluginval loads the plugin with no network and various sample rates and block
  sizes: no network calls in constructors, and racks must handle any
  `prepareToPlay` arguments.

### 10.3 Headless test tool `vibesynth_cli` **[MVP]**
A console target sharing the engine code, with no DAW or UI:
- `--selftest`: compile and offline-test every built-in preset (used in CI).
- `--render patch.vibepatch out.wav`: render a test phrase to WAV.
- `--batch prompts.txt`: run each prompt through generate → compile → test,
  saving patches + WAVs, and print pass rates (parse / compile / test / retries).
  This is how prompt and recipe changes are evaluated and how API credits are
  spent deliberately.

### 10.4 Licensing
- JUCE 8 is AGPLv3 (or commercial). **The repo is AGPLv3.**
- Dependencies stay permissive: DaisySP (MIT, avoid the DaisySP-LGPL repo),
  WDL/EEL2 (zlib-style), Signalsmith DSP (MIT) when added.
- Keep `THIRD_PARTY_LICENSES.md` up to date as dependencies are added.

---

## 11. Engineering rules

- C++17. `-O3` for engine code. **No `-ffast-math`** on validation, safety or
  guard code (it lets the compiler delete NaN/Inf checks). Prefer no fast-math
  anywhere; it's not needed for performance here.
- NaN/Inf checks use bit-pattern tests, not `std::isnan`, so they survive any
  compiler flags.
- Audio thread: no allocation, locks, logging, exceptions, I/O or
  `std::function` with captures. Threads communicate through SPSC queues and
  atomics only.
- Every milestone must build on Linux and pass CI on all three OSes before the
  next one starts.

---

## 12. Repository layout

```
Pulsar/                         (repo root; product name VibeSynthVST)
├── CMakeLists.txt
├── cmake/                      CPM.cmake, EEL2.cmake, platform helpers
├── .github/workflows/build.yml
├── docs/  SPEC.md  ROADMAP.md
├── third_party/WDL/            vendored EEL2 subset
├── resources/
│   ├── presets/*.vibepatch
│   └── prompts/  system_prompt.md  api_reference.md  recipes/*.md
├── src/
│   ├── PluginProcessor.h/.cpp  voices, MIDI, macros, state, swap protocol
│   ├── PluginEditor.h/.cpp
│   ├── engine/
│   │   ├── VoiceRack.h  MasterRack.h      pre-allocated module slots + meters
│   │   ├── Lexicon.h/.cpp                 native function registration (§5)
│   │   ├── ChaosMath.h                    Lorenz, Duffing, bytebeat
│   │   ├── Utilities.h                    safe_fb, crossover, limiter, DC block, smoother
│   │   └── Oversampler.h
│   ├── patch/
│   │   ├── Patch.h/.cpp                   .vibepatch model + parse/serialize
│   │   ├── PatchEngine.h/.cpp             EEL2 VM creation + compile
│   │   └── PatchValidator.h/.cpp          offline test matrix (§7.1)
│   ├── ai/
│   │   ├── MetaClient.h/.cpp              HTTPS, timeouts, cancel
│   │   ├── PromptBuilder.h/.cpp
│   │   └── ResponseParser.h/.cpp
│   └── ui/  PromptPanel  MacroPanel  NodeView  PresetBrowser  SettingsPanel  CodeView
├── tools/vibesynth_cli/main.cpp
└── tests/
```

---

## 13. Milestones

Each milestone has an exit test. Don't start the next until it passes.

1. **Skeleton.** JUCE 8 VST3 + Standalone builds in the Distrobox; 8 "Macro N"
   parameters; plays a sine on MIDI. *Exit: loads and plays in REAPER.*
2. **CI.** 3-OS matrix builds and pluginval passes. *Exit: green on all three.*
3. **EEL2 spike (highest risk).** Vendor EEL2, compile a hardcoded voice program
   with one registered native function, run it per sample. *Exit: runs with JIT
   on Linux, Windows x64, macOS arm64 + x86_64 in CI (`vibesynth_cli` smoke
   test). If not, switch that platform to EEL2 portable mode and record it.*
4. **Lexicon core.** VoiceRack/MasterRack, DaisySP wrappers, ChaosMath,
   Utilities, auto-metering, normalized clamped API. *Exit: a hand-written
   preset using 6+ functions sounds right in REAPER.*
5. **Patch system & safety.** `.vibepatch` load/save, offline test matrix, trim,
   runtime guard, master limiter, fade-swap protocol with retire queue, state
   persistence. *Exit: a deliberately broken patch is rejected; switching
   presets mid-note doesn't click; reopening the REAPER project restores the
   sound.*
6. **Presets & CLI.** 15 built-in presets; `vibesynth_cli` `--selftest` and
   `--render`; selftest runs in CI. *Exit: all presets pass in CI.*
7. **Voice features.** Mono/legato + glide, pitch bend, mod wheel, voice
   stealing, 2× oversampling. *Exit: reese/sub presets play legato cleanly;
   folding presets audibly alias less.*
8. **AI generation.** MetaClient, PromptBuilder, ResponseParser, self-heal,
   settings panel, `--batch`. *Exit: ≥70 % of a 30-prompt batch produce a
   passing patch within 2 retries.*
9. **Editor UI.** Prompt panel, labelled macros, preset browser, code viewer,
   read-only node visualizer with live meters.
10. **Release polish.** README with Gatekeeper instructions, tagged GitHub
    release, demo video script.
11. **[Stretch] Mutate & Breed** with 4-variant audition.
12. **[LATER]** extra libraries (§5.2 LATER list), editable graph.
