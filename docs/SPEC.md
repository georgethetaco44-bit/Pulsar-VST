# Pulsar: Specification v3

> **Status: in force since Nov 4 2026.** Replaces v2, now at
> `docs/archive/SPEC-v2.md`. v2 was written for a one-week build. This version is
> written for a free public release with no fixed date: quality decides when it
> ships. v2 still holds the detail on features already built, such as the inspector,
> Mutate and the MOD strip, and "SPEC §" references in existing code comments point
> to its section numbers. v3 wins wherever they differ.
> "Pulsar" is a placeholder name until the rename decision in §13 is made.
> Numbers marked *(proposed)* are starting points to adjust.

---

## 1. The product

**What it is.** A free, open-source synth plugin. You describe a sound in plain
words, AI writes it as a small program, and Pulsar tests it and swaps it in
while you play. Then you shape it by hand with knobs, drawn curves and
modulation. You never have to read or write code.

**Who version 1 is for.** Bass music producers: dubstep, drum and bass, halftime,
riddim, UK garage and neighbouring styles. Other sounds (pads, leads, keys, FX)
are included and must be good, but bass must be excellent. When priorities
conflict, bass wins.

**Why someone would choose it.**
- It can make sounds a fixed synth layout can't, because every sound is its own
  program running sample by sample.
- A sound can be reached by describing it, then refined by ear.
- It is free, with nothing locked.

**What we must not claim.**
- That other tools can't do audio-rate modulation. VCV Rack, Bitwig Grid,
  Reaktor Core and Max gen~ all can.
- "Zero-delay feedback". Ours is one-sample feedback.
- "Genetic algorithm". Mutate is AI-guided variation with the user choosing.
- That AI output is always good. It isn't, and the product is built around
  auditioning and keeping what works.

---

## 2. Decisions this spec is built on

| Decision | Choice |
|---|---|
| Core idea | Describe, then shape. AI builds structure; people refine it without code |
| Audience | Bass music producers first |
| Price | Free. Optional donation link; no nag screens, nothing locked |
| Licence | AGPLv3 (required by the JUCE licence we use) |
| AI access | Users bring their own key or local model. Pulsar runs no servers |
| AI services | Any compatible service, with a short tested list (§7) |
| Sound before launch | The launch waits until the sound bar (§4) is met |
| Dates | None. Phases end when their exit tests pass |

---

## 3. Version 1 scope

**In**
- VST3 and Standalone on Windows x64, macOS (arm64 and Intel) and Linux x86_64.
- Generate, Takes, Mutate, Change this node, Undo and History (all built).
- Node inspector, macros and macro ranges, MOD strip (all built).
- Stereo voices, unison, drawn curves, loadable wavetables (in prototype).
- No-code controls for those new engine features (§5.3).
- A preset library the developer has personally approved (§4.2).
- Full use without a key, except for the AI features (§5.1).

**Not in version 1** (revisit after release)
- Hand-building a patch from an empty page, or rewiring modules. This is the
  most likely next major feature.
- A code editor. The Code tab stays read-only.
- Reference-sound matching (drop in a WAV and match it).
- Breed, the "How I understood it" plan card and web lookup.
- AU, AAX and CLAP formats. Without AU, Logic Pro users can't load it; the README
  says so plainly. AU is the first format to consider afterwards.
- Installers and paid code signing, unless the beta shows they are needed (§9).
- Extra DSP libraries unless a listening test shows a specific need.

---

## 4. The sound bar

This is the main release gate. Earlier work passed every automated check while
the developer still heard patches that "fall flat", so **passing validation says
nothing about whether a sound is good, and neither does a text-only AI judge**:
one gave 5/5 to a patch it never heard.

### 4.1 What the measurements show
Compared with a private reference set of the developer's favourite presets from
other synths (bass group, Nov 4 2026):

| Measure | References | Pulsar |
|---|---|---|
| Side level above 200 Hz | about −8 dB | about −45 dB; most AI patches fully mono |
| Level swing (movement) | about 21 dB | 4–6 dB |
| Brightness, frame-to-frame timbre change | about equal | about equal |

The references also rely heavily on drawn, tempo-synced curves, compression and
distortion, and often use chorus and reverb even on basses.

**Update, Nov 4 2026: those numbers aren't enough.** The stereo, unison and curve
prototypes close most of that gap on paper, and the developer still judged them
not very good:

| Measure (riff renders, medians) | 24 reference basses | 4 prototype renders |
|---|---|---|
| Side level above 200 Hz | −7.5 dB | −11.6 dB |
| Timbre change frame to frame | 1.8 | 1.1 |
| Level (RMS) | −13 dB | −19.5 dB |
| Crest factor while playing | 5.8 dB | 7.8 dB |

Swing is not a useful comparison here: rests in the riff inflate it.

**Result, Nov 5 2026: the engine can do it.** One reference bass was rebuilt in
patch code, with no engine changes and within the CPU budget. It met nearly all the
measurement targets, and the developer judged it good by ear. So the bottleneck is
sound-design depth and the tools that reach it, not the core engine. What mattered:

- **Movement shape.** Drawn curves with exponential bends, restarted on each note,
  and an oscillator level that follows the square of its control. Swapping in simple
  LFOs did the most damage of the three ingredients tested.
- **The source.** A wavetable made from audio. A built-in table changed the tone
  drastically, by up to 28 dB in individual harmonics.
- **Effect order.** Distortion first; chorus and a filter next; multiband compression
  late; then reverb; a high-pass at the very end.
- **Multiband compression with its own gain and thresholds per band.** This creates
  much of the forward top end and removes the low band.

This is one sound, with some settings fitted to it, so phase 2's other test patches
still have to confirm it. Rebuild files stay private (§4.3).

### 4.2 Gates
All *(proposed)*. The developer's ear overrides any measurement.

1. **Presets.** About 40 built-ins, at least 25 of them bass. Each one is
   individually approved by the developer.
2. **Generated sounds.** On a fixed set of 20 bass prompts, the developer would
   keep working on at least half of the results.
3. **Side by side.** At matched loudness next to the references, the bass presets
   don't sound flat or narrow to the developer.
4. **Guard rails, not goals.** The median bass preset has side level above 200 Hz
   within 10 dB of the references and swing within 6 dB. The sub stays mono.
5. **Other sounds.** At least 10 non-bass presets that the developer would use.

### 4.3 Rules for reference material
Learn techniques from the references. Never copy them. Their presets, tables,
renders and names stay out of the repo, and no artist or label names go in repo
files, preset names or prompts. Pack licences are unchecked, so treat them as not
redistributable. Factory tables must be original: generated by code or made from
the developer's own audio.

---

## 5. The experience

### 5.1 First run, no key
Opens on a good preset, ready to play. All presets, the inspector, macros, MOD
strip, curves and wavetables work. AI buttons explain what they need and offer
[Set up AI]. Nothing nags. This has to feel like a complete instrument for
someone who never adds a key.

### 5.2 Setting up AI
A short guided panel: pick a service or local model, paste a key if needed, press
Test. It says what a typical generation costs, measured on the tested list and
dated, because prices change.

### 5.3 Shaping without code (new work)
Everything the new engine adds must be reachable without code:
- **Draw curves:** add, move and bend points, with sync rates from 1/32 to 4 bars.
- **Pick wavetables:** browse factory and user tables, and hear the choice at once.
- **Stereo and unison:** voices, detune and width on the inspector's knobs.
- **Assign:** curves work with the MOD strip's drag-to-assign and depth rings.

### 5.4 Working rules
- A failed AI request never stops the current sound.
- Generating never blocks the interface.
- Every user-facing word says the product name, never the codename.

---

## 6. Engine

The architecture from v2 stays; it works and is proven on all targets.

### 6.1 How a sound runs
- A patch is EEL2 code (the sandboxed language behind REAPER's JSFX), compiled to
  native code by a built-in JIT. Sections: `init`, `voice`, `master`.
- 8 sounding voices (10 voice objects, for clean stealing) plus one master. Each
  has its own VM. Voices run at 2× with one half-band decimator on the sum.
- Mono/legato with glide, pitch bend ±2 semitones, mod wheel as `mw`, and `bpm`
  and `beat` from the host.
- 8 macro parameters plus Voice Mode and Glide. Host parameters are never added or
  removed, so automation doesn't break.
- The function list lives in `LexiconTable.h` and `api_reference.md`, which the
  smoketest keeps in sync. This spec doesn't repeat it.

### 6.2 Lexicon rules
Amount arguments are 0–1 and frequencies are in Hz. Where a sign changes the sound
itself, an amount may run −1..1 (`fx_comb_ff` feedback and feedforward), and the API
reference says so. Everything is clamped. Stateful
functions take a literal slot. All state is allocated before playback. Unused
modules cost no CPU. Every function has argument metadata, bounds and NaN tests,
a bench line and an API reference line.

### 6.3 New in v3

| Item | State | Still needed |
|---|---|---|
| Stereo voices (`outl`, `outr`) | Built: lint and validator catch half-written stereo; adds about 6–12 ns per voice | Nothing |
| `osc_unison` (up to 16 voices) | Built: about 85 ns at 4 voices, 150–175 at 16, including the empty voice | Listen to it |
| `mod_curve` drawn curves | Built: about 75–90 ns at any point count, base rounding fixed, exponential bends | Nothing |
| Loaded wavetables (tables 8–39) | Built: load in the background; DC removed, one gain per table, peak 0.9 | Make portable (§6.5). Until a table is published it plays as table 0, so a patch validated or a bounce started in that window (about 3.5 s for 32 tables) uses the wrong table; fix this with §6.5 |
| Note-restarted curves | Built: `mod_curve_note`; it matched the rebuild's hand-coded curves | Nothing |
| Usable reverb | Built: `bus_reverb` rewritten as an 8-line FDN; `size` sets decay 0.3–6 s, wet at about the input's level, equal-power mix; a moving size is read at most every 16 samples. The 10 built-ins that use it keep their mix with new trims, approved by ear Nov 7 2026 | Nothing |
| Flexible comb | Built: `fx_comb_ff` with signed feedback and feedforward; within 0.15 dB of its formula; `fx_comb` unchanged | Nothing |
| Multiband compressor | Built: `bus_mb`, 3 Linkwitz-Riley bands that sum flat, upward and downward per band, settings in `mem[]`; `fx_ott` unchanged. The rebuild's swap to it was approved by ear Nov 7 2026 | Nothing |
| Other effects | Not started | Choose by listening against §4; add one at a time |

### 6.4 CPU budgets
Measured on the Steam Deck at 48 kHz, 2× oversampling:
- Poly: ≤ 300 ns per voice per oversampled sample.
- Mono: ≤ 1,200 ns.
- Master: ≤ 2 µs per sample. Heavy character processing belongs here.

`tools/check.sh strict` enforces these on the Deck. CI only reports them, since
its timings are unreliable. Budgets don't change without a measured reason.

A cost is a range, not one number (measured Nov 5 2026 on the Deck). Some functions
cost 1.5–5× more depending on where their machine code lands, modulo 8 KB. On Linux,
part of that is random at each launch; the rest is fixed per build, and any rebuild,
even a CLI-only change, reshuffles which functions are hit. The CPU clock also dips
after idle, slowing everything about 1.45× for a few seconds. Neither depends on the
patch or the voice's data. So a preset close to its budget can pass or fail by build.
`vibesynth_cli --cost-probe` measures one patch per fresh process,
with an ALU clock check and the `out = 0` baseline as controls.

So the strict check judges the patch, not where one build's code happened to land
(decided Nov 7 2026). It measures each build at several code positions: the CLI
relinked with its code shifted by whole KB, and the launch randomisation turned off.
Each position runs in a fresh process on one core, after a warm-up. A preset passes
on its best position. Its worst position is printed as a note, and so is any preset
whose best figure is within 5 % of its budget. Every other selftest check must pass at
every position. The smoketest runs once, at the build's own position. The plugin binary
itself is never padded. Turning randomisation off also fixes the address of the
patch's compiled code buffer. Whether that buffer's position changes cost hasn't been
measured. The `out = 0` baseline moves with placement too (43 or 119 ns
in one build), so cost figures subtract the usual 43 ns rather than the probe's own reading. *(Proposed)* new
built-ins stay under 90 % of their budget in the slow case.
Rosetta is not usable for real-time work (about 2,435 ns measured); the README
tells Apple Silicon users to run their DAW natively.

### 6.5 Patch format and the compatibility promise (new)
After the first public release, **a saved sound must keep sounding the same.**
- `.pulsar` files carry a format version and old versions always load. Unknown keys
  are ignored.
- Each patch records the engine version it was made with. If a function's sound
  must change, the old behaviour stays available for patches that used it.
- Golden renders of every built-in run in CI. Any unintended change fails.
- Host parameter IDs and plugin identifiers freeze at the first public release.
- Tweaks, macro maps, mods, curves and wavetable references all round-trip through
  files and DAW sessions.

**Wavetables must travel with the sound.** Today a patch stores only a table
number, so it breaks on another machine. Factory tables get permanent IDs. A user
table is stored by name and content hash, and exported patches embed it. The
256 KB patch size limit is raised for embedded tables only. A patch whose table is
missing still loads, falls back to table 0 and says so.

---

## 7. AI

### 7.1 Services
The current client speaks the common chat-completions format, with a configurable
address and models. Version 1 turns that into service profiles:

- **Profile:** name, address, model for each use, key or no key, timeout, token
  limit, and optional settings. `reasoning_effort` is sent only when the profile
  says the service accepts it.
- **Tested list:** Meta plus at least one other cloud service and one local model
  server *(proposed)*. A profile earns "tested" by passing §7.4.
- **Custom:** any compatible address, labelled "untested; results will vary".
- **Local models:** timeouts must be much longer than the current 60 s, and the
  roughly 7k-token prompt must fit their context.
- **To verify:** plain-HTTP localhost on macOS inside a DAW. The host app's network
  rules may block it, so test this before promising local use on Mac.
- Rename `MetaClient` to a neutral name, and remove "Meta" from the key prompts in
  the UI. Services outside this format are out of scope for version 1.

### 7.2 Keys
- Stored in the user's app-data settings file, in **plain text**. The setup panel
  and README say this directly. OS keychains can come after 1.0.
- Never in the repo, in patches, in DAW sessions or in logs; tests check this.
- `VIBESYNTH_API_KEY` overrides for development.

### 7.3 Prompt work
- Remove rules the references contradict: "no reverb on basses" and the near-mono
  guidance. Teach width above a mono sub, and drawn tempo-synced movement.
- Teach the §4.1 techniques: movement shape, effect order, multiband compression and
  a level that follows the square of its control.
- Replace agent-written examples with presets the developer approved. The 12
  agent-written 14f candidates don't ship.
- Keep what works: full API reference in every prompt, lexicon lint, self-heal with
  at most 2 retries, and category-based example selection.

### 7.4 Measuring AI quality
- **Automated (new):** render each result and measure width, swing, flatness and
  level against §4. A clear miss on a generated patch feeds back through self-heal
  once. Text-only judge scores are a rough diagnostic and never a gate.
- **Tested label:** at least 90 % of the 30-prompt batch passes within 2 retries
  *(proposed)*, and the developer listens to the bass prompt set.
- **Release gate:** §4.2 item 2, on the default service.

---

## 8. Safety and trust

### 8.1 Offline test (before any new or loaded patch plays)
- 9 configurations: notes 28, 40, 55 × macros at 0, default and 1. One second each.
- Reject on: NaN or Inf (bit-pattern tests); peak above +12 dBFS before the limiter;
  DC above 0.1 over the last 0.5 s; all configurations silent; or over 50 % of real
  time. New: in any configuration, one channel below −80 dBFS while the other sounds
  and is more than 10× louder. A hard pan held at a macro extreme is rejected; a
  moving autopan passes.
- Loops capped at 1,024 iterations and nesting at 2. Sections ≤ 32 KB. Per-sample
  deadline in the validator. Trim only attenuates, to −3 dBFS worst-case peak.

### 8.2 Runtime guard (always on)
No denormals. A voice with non-finite output is zeroed, reset and muted for 20 ms.
The master always ends with trim, DC block, peak limiter (−0.3 dBFS, no latency)
and hard clip. Final output is checked for non-finite values.

### 8.3 Swapping
5 ms fade out, swap, 5 ms fade in, with held notes kept. Programs move between
threads only through lock-free queues. The audio thread never frees a program.

### 8.4 Real-time rules
On the audio thread: no allocation, locks, logging, I/O, exceptions or capturing
`std::function`. No `-ffast-math` on validation, safety or guard code.

### 8.5 Shared files are untrusted (new)
Once people trade presets, every `.pulsar` and `.wav` is input from a stranger.
Before release, audit and fuzz the patch parser and wavetable loader against
malformed, oversized and hostile files. Confirm that the lint and sandbox block
file and system access. Any patch from a file goes through the full offline test.

### 8.6 Privacy (new)
- No telemetry, accounts or update pings. Pulsar touches the network only when the
  user runs an AI feature or presses Test.
- Only the prompt, the active patch and instructions are sent to the service the
  user chose. Audio is never sent. If "Learn from my presets" is on, those presets
  are sent too, and the UI says so.
- The README states all of this.

---

## 9. Distribution and licensing

- **Releases:** GitHub Releases, one zip per OS, from `v*` tags through CI. Each
  build must pass selftest and pluginval.
- **Signing:** none at 1.0 *(proposed)*. macOS builds are ad-hoc signed, and the
  README explains the Gatekeeper steps; Windows users will see a SmartScreen warning.
  Revisit if the beta shows this loses too many people, or if donations can cover
  the yearly cost.
- **Testing without Mac or Windows hardware:** CI proves builds run, not that they
  work in a DAW, so human testers are required (phase 7).
- **Licence duties:** AGPLv3 text and a source link in each zip and in the About
  panel. `THIRD_PARTY_LICENSES.md` is kept current. Dependencies stay permissive,
  apart from JUCE. Check VST3 naming and logo rules before release.
- **Stated plainly in the README:** the AGPL covers Pulsar's code, and sounds and
  music you make with it are yours.
- **Donations:** a link in the README, README-INSTALL.txt, GitHub Sponsors and
  Settings. Nothing more.

---

## 10. Engineering rules

- C++17, CMake + Ninja, JUCE 8 via CPM. Builds run in the `juce` distrobox; use
  `tools/check.sh`.
- Every merge passes CI on all three OSes.
- Pre-push: `tools/check.sh strict` on the Deck.
- Before a merge: pluginval at strictness 5, installed outside the repo. `tools/check.sh`
  doesn't run it. Steinberg's own validator hasn't been run yet.
- `vibesynth_cli` remains the headless test tool: `--selftest`, `--smoketest`,
  `--render`, `--bench`, `--batch`, `--bake`, `--macro-scan`, `--judge`,
  `--wavetable-load-test`.
- Include-path rule: no extensionless files or names matching standard headers.
- Developer-only tools are fine as long as users never need them.

---

## 11. Current state (checked Nov 7 2026)

- The dev machine now runs Nobara Linux on the same Deck hardware. The `juce` distrobox
  was rebuilt, and builds and tests run as before.

- `main` now holds the finished engine prototype. CI passed on all three systems,
  so the macOS smoketest problem from the old release plan is resolved.
- No releases or tags exist yet, so plugin identifiers can still change freely.
- The §6.3 prototype fixes are done, and the built-ins matched the previous `main`
  byte for byte. The parked 14f recipes came with the merge; review them before launch.
- 23 built-ins, 43 recipes and 11 examples exist. Most need review against §4.
- README and `docs/DEMO.md` still contain hackathon wording.

---

## 12. The plan

Each phase ends with a test. **"You" below means the developer, who decides by ear
and by using the plugin. No phase requires writing code by hand.**

### Phase 1: Find out why it falls flat (done Nov 5 2026)
**Rebuild test: done Nov 5 2026 (§4.1).** The engine can reach the reference, so work
shifts to tools, sounds and what the AI is taught.

§6.3 prototype fixes: done. pluginval passes at strictness 5, CI passed on all three
systems, and the work is merged.
- *Exit:* built-ins still match main; budgets and CI pass; pluginval has really run.

### Phase 2: Finish the engine
§6.3 sound tools: reverb, flexible comb and multiband compressor built, and approved
by ear Nov 7 2026; the strict check is moving to measuring at several code positions
(§6.4). Next: the compatibility system (§6.5), portable wavetables,
original factory wavetables made from audio, and the audio measuring tool (§7.4).
Whole-process cost jumps: studied Nov 5 2026, see §6.4. The cause is code placement plus
clock dips, not the voice's data, and there's no small fix. Bloom Pulse's 689 ns wasn't
reproduced (worst 428 in 600 processes). Linking Linux builds with 64 KB segment alignment
makes the cost the same at every launch; it's untested in a host, so decide before
release. Also: replace the older bounds tests' `sqrt(-1)`, which is 1 in a patch rather than NaN,
with real NaN written to memory.
- *You:* A/B each new tool and each technique test.
- *Exit:* hand-tuned test patches for wobble, reese, square/riddim, FM and growl
  meet the §4.2 guard rails, and you'd use them.

### Phase 3: No-code shaping
The §5.3 controls, and §5.1's no-key experience.
- *You:* take a preset to a new finished sound, using only the plugin in Bitwig.
- *Exit:* you did that without touching code or asking for help.

### Phase 4: Preset library
You design and approve presets using AI plus the new shaping tools. Approved ones
become built-ins and AI examples via `--bake`. Old presets that don't measure up
are cut.
- *You:* this phase is mostly yours, and it's the largest.
- *Exit:* §4.2 items 1, 3, 4 and 5 pass.

### Phase 5: AI quality and services
Service profiles, setup panel, prompt rewrite and render-based checks. Test each
service on the list.
- *You:* listen to the 20-prompt bass set.
- *Exit:* §4.2 item 2 passes on the default service; every "tested" service earns
  its label.

### Phase 6: Make it public-ready
Apply the chosen name to everything (§13). Rewrite the README and DEMO.md without
hackathon wording. Write setup, cost and privacy docs. Add donation links. Run the
file security audit (§8.5). Remove internal working-rules files from the repo and
check the repo for AI-tool names.
- *Exit:* a new person can install, play and set up AI from the docs alone;
  no hackathon or old-name references remain; audit findings are fixed.

### Phase 7: Beta
Tagged pre-release with a small group of testers *(proposed: at least 5, covering
Windows, macOS and Linux)* in Ableton Live, FL Studio, Bitwig and REAPER.
- *You:* recruit testers from communities you know.
- *Exit:* no open crash or data-loss bugs; sessions reopen sounding the same on
  every OS tested.

### Phase 8: Version 1.0
Freeze identifiers, tag v1.0.0, publish, launch video.
- *Exit:* the release is live, and every feature in the video works in that build.

### Afterwards
Listen to users first. Likely candidates: hand-building patches from empty,
reference-sound matching, AU format, signing and installers, preset sharing.

---

## 13. Open decisions

| Decision | Needed by | Notes |
|---|---|---|
| **New name** | Before phase 6 | Decided: rename. The name "Pulsar" overlaps with an established plugin company and a sound library. Check candidates for plugin-market, domain and GitHub conflicts. Consider a trademark search |
| Patch file extension and app-data folder | With the name | Keep reading `.pulsar`; move existing settings once |
| Final plugin identifiers | With the name | Bundle ID, manufacturer code and plugin code freeze at the first public binary |
| Numbers marked *(proposed)* | Before the phase that uses them | Your call |
| Tested service list | Phase 5 | Based on results and cost |
| Code signing | After beta | Cost against lost users |
| Donation platform | Phase 6 | GitHub Sponsors is already planned; others optional |
