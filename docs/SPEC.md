# Pulsar — Specification (v2)

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
- **Naming:** everything a user sees is **Pulsar**: the plugin name in the DAW,
  the `.vst3` bundle, the Standalone app, window titles, release zips, the
  app-data folder and the README. "VibeSynth" is the internal codename and may
  appear only in code: the CMake target, class names, `vibesynth_cli` and the
  `VIBESYNTH_API_KEY` env var.
- **Repo:** https://github.com/georgethetaco44-bit/Pulsar-VST (public, AGPLv3).

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
                               4. Compile EEL2 into 11 fresh VMs
                                  (10 voice objects + 1 master)
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
  Master runs at 1×. Voices generate at 2× directly (no upsampler), and the
  voice sum is mono, so **one** decimator runs on the summed 2× signal, not
  one per voice: an own polyphase-IIR half-band (allpass pair; ≥ 90 dB
  rejection above 0.55 × host rate, passband flat to 20 kHz at 48 kHz). The
  validator, `--render` and `--bench` use the same 2× path; `vibesynth_cli`
  keeps a `--os 1` switch for A/B aliasing measurements.
- Any user-created variable persists between samples (that's how state works).
  A newly loaded patch starts with all variables at 0.
- VM memory (`mem[]`, for user delay lines or tables) is capped per VM
  (e.g. 65,536 slots).
- **EEL2 allocates `mem[]` lazily** (`calloc` on first access, inside
  `__NSEEL_RAMAlloc`), which would mean an allocation on the audio thread.
  Every VM must call `NSEEL_VM_preallocram(vm, -1)` after
  `NSEEL_VM_setramsize`, off the audio thread, before it is ever executed.
  Named `gmem` is not exposed to patches.

### 4.2.1 CPU budget (from Milestone 3 measurements)
- Measured cost of one `NSEEL_code_execute` running `out = osc_sine(0, freq);`:
  Steam Deck 66 ns/sample, CI Linux 54, Windows 49, macOS arm64 69. Almost all
  of that is fixed per-execute overhead, not the sine.
- **Budget: the whole voice section (8 voices, 2× oversampling, 48 kHz) uses at
  most 25 % of one Steam Deck core.** That is **≤ 300 ns per voice per
  oversampled sample**, including the ~66 ns execute overhead, leaving about
  230 ns for lexicon calls. The master VM gets at most 10 % (≈ 2 µs per
  sample), which is where reverb and chorus live.
- Lexicon functions should be designed so that a typical patch (one or two
  oscillators, a filter, an envelope, a shaper, feedback) fits the budget. Each
  function's cost is benchmarked in `vibesynth_cli`.
- Milestone 4 measured on the Deck: the execute baseline is ~32 ns. Every single
  lexicon function fits; the heaviest are `flt_ladder` (+178 ns, because DaisySP
  oversamples it 4× internally), `phys_resonator` (+145) and `phys_string`
  (+112). The first test preset (supersaw + saw + sine + ladder + adsr + sat +
  fold + safe_fb) cost 518 ns, so a full patch, not any one function, is what
  breaks the budget.
- Performance rules for wrappers:
  - Cache parameter mappings on their **raw 0–1 inputs**, before any `pow`, so
    a mapping is only recomputed when its input changes.
  - Coefficient-heavy setters (filter cutoff/res, envelope times, resonator) run
    at **control rate**: update every 16 samples, with linear interpolation of
    the mapped value where zipper noise would be audible.
  - Use cheap approximations (rational tanh, polynomial sin) where they are
    inaudible, and precompute any per-sample `exp`.
- After these rules the test preset costs 283 ns with `flt_lp` but still 413 ns
  with `flt_ladder` (the ladder alone is ≈144 ns per call). The default preset
  therefore uses `flt_lp`. In Milestone 7, when voices run at 2×, DaisySP's
  internal 4× ladder would become 8×. So M7 replaces it with our own 1× ladder,
  and probably our own TPT SVF too, whose coefficients can be interpolated
  per sample. DaisySP's Svf can only step its cutoff every 16 samples.
- Headroom is thin for **poly** patches: about one filter plus one shaper
  plus an oscillator stack. The M8 prompt examples must stay that lean.
- **Budget by voice mode (M6 amendment).** The 25 % budget assumes 8 sounding
  voices. A `"voiceMode": "mono"` patch only ever sounds one voice (plus a
  2 ms steal fade), so its voice section may cost **≤ 1,200 ns per oversampled
  sample** (4× the poly figure, and still only ~12 % of a Deck core at 2×).
  Poly patches stay at ≤ 300 ns. Until M7 enforces mono, a mono patch played
  as a chord can overrun; that's accepted for the M6–M7 gap.
- **The master is the cheap place for heavy processing.** It runs once, not
  per voice, and its 2 µs budget was ~5 % used after M6. Character processing
  that suits the bus (distortion chains, multiband, combs, chaos-modulated
  filters) belongs in `master`, which can call the whole lexicon (§5.2).
- The Deck bench is noisy (cheap rows vary up to 2× between runs), so judge by
  the median of several runs and by absolute ns. The CPU clock ramps from idle
  (the first run is ~45 % slower), so every timed measurement starts with one
  untimed warm-up pass.
- **Where the budget is enforced:** `vibesynth_cli --selftest --strict-cpu` on
  the Deck fails any preset over its §4.2.1 budget. In CI (shared, variably
  clocked runners, Rosetta) the per-preset ns are reported but never gate;
  CI gates only on the §7.1 runaway rule. The `out = 0` baseline shifts
  with the build layout (32 vs 68 ns), so "vs base" is unreliable.
- macOS x86_64 under Rosetta measured 2,435 ns/sample (Rosetta translates
  JIT-generated code slowly). It is correct but not usable for real-time work.
  The README tells Apple Silicon users to run their DAW natively, not under
  Rosetta. Real Intel Macs run the same x86_64 JIT as Linux/Windows.

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
| `mw` | mod wheel 0–1, smoothed like the macros (M7) |
| `fb1`, `fb2`, `prev` | conventions for one-sample feedback and zero-crossing detection (plain variables) |
| `bpm` | host tempo (M6 amendment; 120 when the host gives none) |
| `beat` | position in quarter notes, advanced per sample (host PPQ while playing; free-running from the last position when stopped), for tempo-synced LFOs, wobbles and stutters, e.g. `lfo = sin(beat * 2 * $pi * 2)` for 1/8 notes |

| Master VM | Meaning |
|---|---|
| `inl`, `inr` | summed voice output |
| `outl`, `outr` | **write**: bus output (default passthrough) |
| `m1`…`m8`, `srate`, `bpm`, `beat` | as above |

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

**Master VM (M6 amendment):** the master VM owns its own `VoiceRack` as well
as the `MasterRack`, so every voice function below (`osc_*`, `flt_*`, `fx_*`,
`env_follow`, `util_*`, `chaos_*`, `phys_*`) also works in `master`, with
its own slots. `gate`, `freq` and `note` aren't bound in `master`, so `env_adsr`
there needs a gate the patch makes itself. `bus_*` functions stay master-only.

**[MVP] core (DaisySP + our own code):**

| Function | Source | Notes |
|---|---|---|
| `osc_sine(slot, hz)` `osc_saw(slot, hz)` `osc_tri(slot, hz)` | DaisySP Oscillator (PolyBLEP) | 4 slots |
| `osc_square(slot, hz, pw)` | DaisySP | pw 0–1 |
| `osc_supersaw(slot, hz, detune, mix)` | own (7 PolyBLEP saws) or DaisySP | 2 slots |
| `noise()` | own | stateless white noise |
| `flt_lp(slot, in, cutoff, res)` `flt_hp(...)` `flt_bp(...)` | own TPT SVF (M7; `Filters.h`) | cutoff 0–1 mapped exponentially 20 Hz–20 kHz, smoothed per sample; linear; res 1 = Q 20; 3 slots |
| `flt_ladder(slot, in, cutoff, res)` | own 4-pole ZDF ladder (M7; `Filters.h`), Padé tanh in the feedback | self-oscillates from res ≈ 0.95, bounded; 2 slots |
| `env_adsr(slot, gate, a, d, s, r)` | DaisySP Adsr | times mapped 1 ms–10 s; 3 slots |
| `env_follow(slot, in)` | own | 2 slots |
| `fx_fold(in, amount)` | own (sin-fold) | stateless |
| `fx_sat(in, drive)` | own (tanh with gain compensation) | stateless |
| `fx_crush(slot, in, bits, rate)` | DaisySP Decimator | 2 slots |
| `fx_comb(slot, in, hz, feedback)` | own fractional delay | 2 slots |
| `fx_ott(slot, in, depth)` | own (M6): 3-band (≈120 Hz / 2.5 kHz) upward + downward compressor, the "OTT" staple of modern bass music; depth 0 = passthrough, 1 = full; fixed fast attack/release; output gain-compensated | 2 slots; works in master (one slot per channel) |
| `phys_resonator(slot, exciter, hz, structure, brightness, damping)` | DaisySP Resonator | 1 slot |
| `phys_string(slot, exciter, hz, brightness, damping)` | DaisySP KarplusString | 1 slot |
| `chaos_lorenz(slot, rate, axis)` | own ChaosMath.h (RK2, ≤4 substeps) | rate 0–1 maps exponentially to an orbit rate of 0.05 Hz–1 kHz; axis 0/1/2; initial state seeded per voice; 2 slots |
| `chaos_duffing(slot, hz, chaos)` | own ChaosMath.h (forced Duffing, ±cos forcing, DC-blocked) | hz sets the forcing period (capped at ~0.076·sr); chaos 0–1 sets forcing strength 0.20–0.50; 2 slots |
| `util_safe_fb(slot, x)` | own | 20 Hz DC block + slew limit + tanh. All feedback must pass through this |
| `util_xover_lo(slot, in)` / `util_xover_hi(slot)` | own Linkwitz-Riley 4th order @ 100 Hz | `_hi` returns the high band of the last `_lo` call |
| `util_bytebeat(t, mode)` | own | bitwise helpers; integer ops done in C++ |

**[MVP] master bus functions:**

| Function | Notes |
|---|---|
| `bus_sat(in, drive)` | stereo-safe saturation (call per channel) |
| `bus_mono_low(l, r)` / `bus_mono_low_r()` | sums sub-100 Hz to mono, returns new L (and R via second call) |
| `bus_chorus(l, r, depth, mix)` / `bus_chorus_r()` | own or DaisySP Chorus; returns new L, R via the second call (same pattern as `bus_mono_low`) |
| `bus_reverb(l, r, size, mix)` / `bus_reverb_r()` | own small FDN (DaisySP ReverbSc is in the LGPL repo, so don't use it); returns new L, R via the second call |

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
- Include 2 complete example patches as few-shot examples. These are
  **separate, lean teaching files** in `resources/prompts/examples/` (M8),
  not the built-in presets. The presets are a showcase and may be as rich as
  the budget allows; the examples must stay within the poly budget and be easy
  to imitate.
- Files live in `resources/prompts/` and are embedded with JUCE BinaryData.

### 6.2 Response format
Ask the model for exactly this layout. Don't put code inside JSON strings; the
escaping breaks.

````
```json
{
  "name": "Weeping Reese",
  "macros": [ {"label": "Detune", "default": 0.4}, ... 8 entries ... ],
  "voiceMode": "mono",
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
OpenAI-compatible) are configurable in settings, not hardcoded. The model is a
reasoning model: requests send `reasoning_effort: "low"` (settings file only)
and `max_tokens: 8192`; a reply that spends the whole cap reasoning is its own
failure reason ("hit the token limit while reasoning") and is retried, as are
empty, garbled or timed-out replies. Measured in M8: ~13 s per generation
(max 25.8 s), ~5.1k prompt + ~2.4k completion tokens (5.4k prompt after Sound pass batch 1). Timeout is **60 s**.
The UI shows progress, never blocks.

### 6.4 Self-heal
On parse, compile or test failure, send the model its own output plus the exact
reason (e.g. `EEL2 compile error line 7: ...`, or
`NaN produced at note=40, m3=1.0 after 0.31 s`). Maximum 2 retries, then report
failure in the UI and keep the current patch playing.

**M8 additions:**
- **Lexicon lint before compile.** Every `name(` call in the code is checked
  against the registered lexicon plus EEL2 built-ins and the patch's own
  `function` definitions. An unknown name fails with the closest valid names
  ("unknown function `osc_wavetable`; did you mean `osc_saw`, `osc_square`?
  available osc_*: …"), because invented functions are the most common LLM
  failure. Arity and literal slot ranges are checked the same way.
- **Weight hint.** If a patch passes but its measured voice cost is over 1.5×
  its §4.2.1 budget (poly/mono) and a retry is left, retry once with "too
  heavy: N ns per voice-sample, budget B; simplify". If no retry is left,
  accept it. CPU is never a hard reject beyond the §7.1 rule.
- **Single source for the API reference.** `resources/prompts/api_reference.md`
  is checked by the smoketest against the registered lexicon: every
  registered function documented with the right arity, nothing documented
  that isn't registered.
- **Token use** (prompt + completion, from the API's `usage`) is logged per
  request in `--batch`, never the key or full headers.

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
- CPU over budget: one configuration (voice + master) takes more than 50 % of
  real time to render (a runaway guard, deliberately much looser than the
  §4.2.1 design budget, so patches still load on slower machines than the
  Deck). M6 raised this from 10 %: a mono patch spending its full 1,200 ns
  voice and 2 µs master budget on a cold Deck CPU needs ~220 ms per
  configuration, more at 2× oversampling (M7), so a 10 % limit would reject
  legal patches.

Target (report-only, warm Deck): the whole matrix finishes in under 300 ms
for poly patches and under 600 ms for mono patches.

**Runaway code (M5).** JIT code can't be interrupted mid-sample, so:
- EEL2 is built with `NSEEL_LOOPFUNC_SUPPORT_MAXLEN=1024` (the default is
  1,048,576), so one `loop`/`while` runs at most 1024 iterations. Verify this
  on every JIT backend, because Windows x64 links prebuilt asm objects.
- The validator checks a wall-clock deadline (and its cancel flag) after every
  sample and rejects the patch ("too slow") as soon as one configuration has
  used more than 500 ms (50 % of its 1 s of audio, matching the CPU rule), so
  it never waits out a slow patch. The per-sample clock read costs ~10 ms over
  the whole matrix; it runs only in the validator, never on the audio thread.
- Before compiling, a text check rejects `loop`/`while` nested more than 2
  deep (a loop inside a user function counts as one level wherever that
  function is called). With the 1024 cap, one sample is then at most ~1M
  iterations (a few ms), so the deadline always fires promptly and the worker
  can always be stopped.
- EEL2 already zeroes NaN/Inf/denormals on most variable stores, so many bad
  patches never produce NaN; the guard still catches the store paths that
  don't. `OPTFLAG_FULL_DENORMAL_CHECKS` stays off.
- Size limits: each code section ≤ 32 KB, the whole file ≤ 256 KB.

Compile errors (any section) are rejected before the matrix runs, with the
EEL2 error text and section name kept for self-heal (§6.4) and the UI.

**Trim:** compute one gain so the worst-case peak lands at −3 dBFS. The trim may
only attenuate (gain ≤ 1.0), never boost.

### 7.2 Runtime guard (audio thread, always on)
- `juce::ScopedNoDenormals` in `processBlock` (feedback and resonators produce
  denormals).
- After each voice's block: if any sample is non-finite, zero that voice's
  output, reset its rack and VM variables, and mute it for 20 ms.
- Master chain always ends with trim → DC block → peak limiter → hard clip ±1.0.
  The limiter has no lookahead (so no latency): instant attack, ~80 ms release,
  ceiling −0.3 dBFS. It replaces M4's fixed −12 dB output gain.
- Final output is checked for non-finite values and zeroed if found.
- "Reset its VM variables" means: zero every variable found via
  `NSEEL_VM_enumallvars` (a pointer list collected off the audio thread at
  compile time; the bound inputs get rewritten anyway) and zero `mem[]`
  through `NSEEL_VM_getramptr_noalloc`. No allocation.

### 7.3 Hot-swap protocol
A `PatchProgram` owns everything compiled from one patch: 10 `EelVoice`s
(VM + rack each), one `EelMaster`, the trim, and the parsed patch (macros,
voice mode, graph, source). Voice *slot* state (note, age, gate, fade) stays
in the processor, so it survives a swap. The program is built for one sample
rate; a sample-rate change in `prepareToPlay` rebuilds it synchronously.

1. Worker pushes the new `PatchProgram*` on the swap queue.
2. Audio thread fades the output out over 5 ms.
3. At silence it swaps programs, resets all racks (no allocation; reset only),
   and pushes the old program on the retire queue.
4. Fades in over 5 ms. Held notes keep sounding with the new patch: each
   held slot gets a note-on on its new voice (so `trig` = 1 and `init` runs);
   released voices are dropped.
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

**Glitch micro-edits.** Hashed per-1/16 decisions (repeat, reverse-ish
re-trigger, pitch jump, mute), each switch smoothed with a ~0.3 ms slew so
edits never click. Applied to the mid band only. (Replaces the M6
wave-surgery recipe.)

**Piece-wise transfer functions & chaos modulation.** Custom shapers inline;
chaos at LFO rate (`rate` near 0) or audio rate (near 1).
```
c = chaos_lorenz(0, m5 * 0.2, 0);
x = osc_saw(0, freq * (1 + c * m6 * 0.05));
out = x > 0 ? sin(x * (1 + m1 * 8)) : floor(x * 4) / 4;
```

**Mix-clean rules (Sound pass batch 1).** These hold for every recipe and are
in the system prompt:
- The sub is a clean sine added **last**, summed to mono. Nothing distorts it,
  and no gain is ever computed from the low band (the old
  `max(0.15, 1 - abs(lo))` ducking caused sub-driven mid-band clipping; the
  coupling check measured it at −11 to −15 dB).
- The mid band is built on its own, staged (drive → filter → fold → OTT →
  fixed ceiling), and high-passed at ~130 Hz.
- OTT goes after distortion, at most 0.3 deep on decaying sounds.
- One strong idea over many layers; releases 15–40 ms; every macro audible and
  m1 transforms.
- `bus_sat` never runs on a signal that contains the sub.
```
mid = fx_fold(flt_lp(0, fx_sat(osc_saw(0, freq), 2 + m1 * 6), m2, m3), m4);
mid = flt_hp(1, mid, 0.27, 0);   // 0.27 ≈ 130 Hz
out = (fx_ott(0, mid, m5 * 0.5) * 0.7 + osc_sine(0, freq) * 0.5) * env_adsr(0, gate, 0.002, 0.2, 0.9, 0.02);
```
(Illustrative only; the recipes in `resources/prompts/recipes/` are the
rendered, checked versions.)

---

## 9. Plugin behaviour & UI

### 9.1 Voices **[MVP]**
- 8-voice polyphony, plus **mono/legato mode** with glide time (bass patches are
  mostly mono). Patch JSON may set a default mode; the user can override.
- Voice stealing (M4 amendment): 10 voice objects, at most 8 sounding. On a
  9th note the victim (the oldest released voice, else the oldest held one)
  fades out linearly over 2 ms while the new note starts at once on a spare.
  Only when no spare is free (10 note-ons within 2 ms) does a voice
  hard-steal in place: a 2 ms fade, then retrigger. No note-on is dropped.
- Pitch bend ±2 semitones; mod wheel is readable as variable `mw` (0–1).
- **M7 details.** Two non-macro host parameters, added at startup and never
  removed: "Voice Mode" (Patch / Poly / Mono; Patch follows the patch's
  `voiceMode`) and "Glide" (0–1 → glide² seconds, so 0.3 → 90 ms and
  1 → 1 s; constant-time, linear in pitch; used in mono only). Mono is last-note priority with a held-note stack: releasing the top
  note returns to the previous held one. Legato: a note-on while another is
  held glides `freq` without retriggering (`gate` stays 1, `trig` stays 0,
  `init` doesn't run). A note after all keys are up retriggers the same voice
  in place (`trig` = 1, `init` runs, no steal fade). Pitch bend is smoothed
  (~5 ms) and applied to `freq` after glide.

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
recompiles the saved code locally. **It never calls the LLM.** A restored patch
skips the offline matrix (it passed when it was first loaded) but must still
compile; if it doesn't, Pulsar loads the default preset and shows the error.
The state is the APVTS tree with the patch JSON as a child property, so older
or newer state versions fail soft.

### 9.4 Built-in presets **[MVP]**
15+ hand-tuned patches in `resources/presets/*.pulsar`, covering reese,
neuro, UKG organ/stab, sub, physical-model plucks, chaos textures. They are:
the out-of-the-box demo (a showcase of what only per-sample code can do,
plus solid bread-and-butter sounds) and regression tests. They are not the
LLM few-shot examples (§6.1).

`.pulsar` format:
```json
{
  "version": 1,
  "name": "Weeping Reese",
  "prompt": "the original user prompt",
  "macros": [{"label": "Detune", "default": 0.4}],
  "voiceMode": "mono",
  "graph": {"edges": [["osc_supersaw:0", "flt_lp:0"]]},
  "code": {"init": "", "voice": "...", "master": "..."},
  "trim": 0.62
}
```
`trim` (M5) is the gain the offline test computed, saved so that restoring a
project or picking a built-in needs no matrix. On load it is clamped to
(0, 1]; any patch loaded from a file or generated is re-tested, and the
computed trim replaces the stored one. Built-ins are compile-only on
selection, because CI's selftest checks that each stored trim matches the
computed one within 1 dB (FMA on arm64 and chaotic or bit-crushed signals
make the exact peak differ between platforms; a 1 dB trim difference is
inaudible as a mismatch, and every platform still runs the full matrix).
Patches must not be shaped to hit a trim of exactly 1.0 for CI's sake.
Macro defaults and trim are rounded to 1e-6 so
the file round-trips exactly.
Loading a patch file runs the same compile + offline test as a generated patch.

### 9.5 Editor **[MVP]**
- Prompt box, **Generate** button, status/progress line, error display.
- 8 macro knobs with dynamic labels.
- Preset browser (built-ins + user-saved patches).
- Settings panel: API key, model IDs, and a **Test key** button (one tiny
  request; an HTTP 200 that parses as chat JSON counts as "Key works", even
  with an empty message).
- **First run:** with no key, the prompt row becomes a card ("Add your Meta
  API key to generate sounds" + [Add key], which opens Settings on the key
  field). Saving a key restores Generate immediately.
- Unused macros (never read by the code, per `macrosRead`) are dimmed with no
  label or value; tooltip "Unused by this patch".
- Code viewer (read-only text) showing the active patch.

### 9.6 Node visualizer **[MVP, read-only]**
- **Nodes** come from the *real* code: the set of (module, slot) pairs touched
  during the offline test.
- **Edges** come from the LLM's `graph.edges`, filtered to nodes that really
  exist. They are labelled as descriptive.
- **Stateless shapers** (`fx_fold`, `fx_sat`, `util_bytebeat`, `bus_sat`) have
  no slot, so each call is numbered by its order within the sample
  ("fx_sat #0", "#1", …; the 4th and later calls share #3). Under branches
  this numbering is descriptive only.
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
- Matrix: `ubuntu-22.04`, `windows-latest` (MSVC 2022 x64), `macos-15` (arm64; moved from macos-14, which GitHub retires on 2026-11-02).
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
- `--render patch.pulsar out.wav`: render a test phrase to WAV. `--phrase
  default|riff` (M6): `riff` is a 2-bar syncopated 16th-note bass riff with
  octave jumps, rests and a hero-macro (m1) automation, so bass presets are
  auditioned the way they'd be used; `--bpm` sets its tempo.
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
Pulsar/                         (repo root; product name Pulsar, codename VibeSynth)
├── CMakeLists.txt
├── cmake/                      CPM.cmake, EEL2.cmake, platform helpers
├── .github/workflows/build.yml
├── docs/  SPEC.md  ROADMAP.md
├── third_party/WDL/            vendored EEL2 subset
├── resources/
│   ├── presets/*.pulsar
│   └── prompts/  system_prompt.md  api_reference.md  recipes/*.md
├── src/
│   ├── PluginProcessor.h/.cpp  voices, MIDI, macros, state, swap protocol
│   ├── PluginEditor.h/.cpp
│   ├── engine/
│   │   ├── VoiceRack.h  MasterRack.h      pre-allocated module slots + meters
│   │   ├── Lexicon.h/.cpp                 native function registration (§5)
│   │   ├── ChaosMath.h                    Lorenz, Duffing, bytebeat
│   │   ├── Utilities.h                    safe_fb, crossover, smoother
│   │   ├── OutputStage.h                  trim → DC block → limiter → clip (§7.2)
│   │   └── Oversampler.h
│   ├── patch/
│   │   ├── Patch.h/.cpp                   .pulsar model + parse/serialize
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
   **Result: passed. JIT on all four targets, no portable fallback needed.**
4. **Lexicon core.** VoiceRack/MasterRack, DaisySP wrappers, ChaosMath,
   Utilities, auto-metering, normalized clamped API. *Exit: a hand-written
   preset using 6+ functions sounds right in REAPER, and its benchmark on the
   Steam Deck is within the §4.2.1 budget.*
   **Result: passed. Test preset (with `flt_lp`) 283 ns per voice-sample on
   the Deck, against a 300 ns budget; it passed REAPER listening; CI green on all three OSes
   (smoketest + pluginval). The ladder is too heavy for the default preset
   (≈144 ns per call), and it gets replaced in M7.**
5. **Patch system & safety.** `.pulsar` load/save, offline test matrix, trim,
   runtime guard, master limiter, fade-swap protocol with retire queue, state
   persistence. *Exit: a deliberately broken patch is rejected; switching
   presets mid-note doesn't click; reopening the REAPER project restores the
   sound.*
   **Result: passed (commit 41a48da). CI green on all three OSes, including the
   1024-iteration loop cap on every JIT backend; REAPER switch, reject and
   restore checks passed. Validator wall time 57–245 ms per preset on the
   Deck. Carried into M6: per-sample deadline, loop-nesting check,
   compile-only built-in selection (§7.1, §9.4).**
6. **Presets & CLI.** 15 built-in presets; `vibesynth_cli` `--selftest` and
   `--render`; selftest runs in CI. *Exit: all presets pass in CI.*
   **Result: passed (infrastructure). 15 presets pass selftest on all four
   targets; `fx_ott`, master VoiceRack, `bpm`/`beat`, mono budget, `--render
   --phrase riff` added. The user judged the sound unfinished (riffs messy), so
   the presets are provisional and are redone in the Sound pass (9b). Keepers
   so far: Reese, Coupled Scream. Suspected causes to check then: 1× aliasing
   (M7 fixes), 16-sample filter stepping (M7 fixes), mid-band clipping driven
   by sub headroom (intermod smear), OTT lifting tails and noise, no
   legato/glide.**
7. **Voice features.** Mono/legato + glide, pitch bend, mod wheel, voice
   stealing, 2× oversampling. *Exit: reese/sub presets play legato cleanly;
   folding presets audibly alias less.*
   **Result: passed. Own polyphase-IIR half-band (−106 dB from 0.55 × host
   rate, 1.37 host samples latency, unreported; 12 ns per host sample for all
   voices). A fold at 2× has 25.4 dB less inharmonic energy than at 1×. Own TPT
   SVF (≤ 29 ns swept, linear, Q floor 20) and ZDF ladder (≤ 51 ns, k = 4.2·res,
   self-oscillates from res ≈ 0.95); the DaisySP Svf and LadderFilter are no
   longer built. Legato, glide, bend and mw verified by smoketest and in REAPER.
   For the Sound pass: phys_string bottoms out at ~94 Hz at a 96 kHz voice rate
   (DaisySP's fixed 1,024-sample delay), so it needs an own string for basses;
   Wonk Dive's ladder no longer self-oscillates at res 0.72. The validator and
   bench drive EelVoice directly (not PatchPlayer) on the same 2× path, so keep
   them in sync whenever the voice path changes.**
8. **AI generation.** MetaClient, PromptBuilder, ResponseParser, self-heal,
   settings panel, `--batch`. *Exit: ≥70 % of a 30-prompt batch produce a
   passing patch within 2 retries.*
   **Result: passed. 30/30 batch prompts produced a passing patch on the first
   try (224k tokens for the run). The lexicon lint, the API-reference
   self-check, and the switch to a reasoning-model request (`reasoning_effort`
   low, 8,192 max tokens) were the decisive pieces. `bus_sat` is also allowed
   in voice. The lint refuses `freembuf`, `stack_*` and `gmem`. Settings live
   in the "Pulsar" app-data folder (`~/.config/Pulsar/` on Linux). Cancel
   takes up to ~1 s on Linux (JUCE's curl loop). A valid patch isn't
   necessarily a good-sounding one: the Sound pass also reviews generated
   output.**
9. **Editor UI.** Prompt panel, labelled macros, preset browser, code viewer,
   read-only node visualizer with live meters. Look: dark with one neon
   accent (user's choice). *Exit: every §9.5/§9.6 feature works in REAPER on
   the Deck; pluginval passes with editor open/close; the user approves the
   look from snapshots and REAPER.* Mutate/Breed (§9.7) is a separate step
   after M9.
   **Result: passed (pending pluginval in CI). Tested in Bitwig; snapshots
   reviewed. Nodes come from the validator's touched set (or a silent probe
   for built-ins/restores); meters reach the UI through a processor-owned
   lock-free bank, never a program pointer. Edges are contracted through
   non-node names. Open UI items for the next round: (1) stateless shapers
   (`fx_fold`, `fx_sat`, …) are invisible in the node view, though they're
   where much of a patch's character lives; (2) unused macros show as
   "Macro N 0.50" and should read as unused; (3) the output meter reads as a
   menu icon; (4) 16 compiler warnings in Generator.cpp, Filters.h and
   PluginEditor.cpp (`userArea` deprecated).**
   **UI round (branch `ui-fixes`): all four items fixed, plus first-run key
   onboarding and Test key (§9.5). Shapers are nodes (§9.6); probe ==
   validator node list for 15/15; filter output bit-identical; zero warnings
   in src/ and tools/. Still open: long node names (`util_safe_fb`,
   `osc_supersaw`) are truncated in their pills.**
9b. **Sound pass.** With M7's DSP (2×, own ladder/SVF, glide) and the M9 UI in
   place, redo the 15 presets against the user's references (modern DnB,
   dubstep, halftime, glitch), adding any core DSP they need first. Work
   in small batches with the user listening to `--phrase riff` renders.
   *Exit: the user keeps ≥ 10 of 15.*
10. **Release polish.** README with Gatekeeper instructions, tagged GitHub
    release, demo video script.
11. **[Stretch] Mutate & Breed** with 4-variant audition.
12. **[LATER]** extra libraries (§5.2 LATER list), editable graph.
