# Pulsar API reference

Patches are EEL2 code (JSFX syntax): `x = expr;`, `a ? b : c`, `(cond) ? (stmt1; stmt2);`, `%` `^` `&&` `||` `!` `==` `!=`, `+=` etc., `$pi`. Every user variable persists from sample to sample per voice and starts at 0 on load. User functions: `function name(a, b) local(t) ( ... );`.

## Sections
- `init`: runs at each note-on, before the first voice sample (reset per-note state here).
- `voice`: runs once per sample per sounding voice at 2x the host rate. Write `out`.
- `master`: runs once per host sample on the stereo sum. Read `inl`, `inr`; write `outl`, `outr` (default passthrough).
After master, Pulsar always applies trim, DC block, a limiter and a clip at ±1.

## Variables
Voice: `freq` (Hz, incl. glide and pitch bend), `note` (MIDI number), `vel` (0-1), `gate` (1 held, 0 released), `trig` (1 on the note-on sample only), `srate` (voice sample rate), `m1`..`m8` (macros 0-1, smoothed), `mw` (mod wheel 0-1), `bpm` (host tempo, 120 if none), `beat` (position in quarter notes; `sin(beat * 2 * $pi * 2)` = 1/8-note LFO), `out` (write).
Master: `inl`, `inr` (the voice sum; mono, so inl == inr), `outl`, `outr`, `m1`..`m8`, `mw`, `srate`, `bpm`, `beat`. `freq`, `note`, `gate` are not set in master.

## Functions
Format: signature · slot pool · where it works (both = voice and master). Amounts are 0-1 and clamped; hz is in Hz, clamped to 0..0.45·srate. Rough cost in ns per voice-sample in brackets where it matters.

### Oscillators (output about ±1)
- `osc_sine(slot, hz)` · 4 slots · both — sine.
- `osc_saw(slot, hz)` · 4 slots · both — band-limited saw.
- `osc_tri(slot, hz)` · 4 slots · both — triangle.
- `osc_square(slot, hz, pw)` · 4 slots · both — pulse; pw 0-1 (0.5 = square).
- `osc_supersaw(slot, hz, detune, mix)` · 2 slots · both — 7 saws; detune 0-1 (up to ±30 cents), mix 0-1 (centre saw to the 6 detuned ones) [90].
- `osc_wt(slot, hz, table, pos)` · 2 slots · both — wavetable oscillator (`table` 0–7: basic, PWM, vocal, bell, sync, comb, fold, growl; `pos` 0–1 morphs) [25].
- `osc_fm(slot, hz, ratio, index, feedback)` · 2 slots · both — 2-op FM oscillator (`ratio` 0–1 → 0.5–16, snaps to integers) [25].
- `osc_sync(slot, hz, ratio, shape)` · 2 slots · both — hard-sync oscillator (`ratio` 0–1 → 1–16× slave, `shape` saw→square) [25].
- `noise()` · no slot · both — white noise ±1.

### Filters (linear; cutoff 0-1 maps to 20 Hz-20 kHz: Hz = 20·1000^cutoff, so key tracking is cutoff = log(hz / 20) / log(1000))
- `flt_lp(slot, in, cutoff, res)` · 3 slots · both — 12 dB lowpass; res 0-1 (1 = Q 20); smoothed per sample [40].
- `flt_hp(slot, in, cutoff, res)` · 3 slots · both — 12 dB highpass.
- `flt_bp(slot, in, cutoff, res)` · 3 slots · both — bandpass.
- `flt_ladder(slot, in, cutoff, res)` · 2 slots · both — 24 dB ladder with tanh in the loop; self-oscillates from res ≈ 0.95 [130].
- `flt_formant(slot, in, vowel, res)` · 2 slots · both — 3-peak vowel filter (`vowel` 0–1 morphs a→e→i→o→u) [65].
- `flt_notch(slot, in, cutoff, res)` · 3 slots · both — 2-pole band-reject (notch) filter.
- `flt_peak(slot, in, cutoff, gain, res)` · 3 slots · both — parametric bell filter (`gain` 0–1 → −18…+18 dB, 0.5 = flat).
- `flt_tilt(slot, in, tilt)` · 2 slots · both — spectral tilt filter (±6 dB around 1 kHz, 0.5 = flat).

### Envelopes and modulators
- `env_adsr(slot, gate, a, d, s, r)` · 3 slots · both — output 0-1; gate on when ≥ 0.5; a, d, r 0-1 map to 1 ms-10 s (0.25 = 10 ms, 0.5 = 100 ms, 0.75 = 1 s); s is a level 0-1.
- `env_follow(slot, in)` · 2 slots · both — envelope of |in| (1 ms attack, 100 ms release).
- `mod_lfo(slot, beats, shape)` · 3 slots · both — tempo-synced LFO (`beats` = period, `shape` 0–1: sine/tri/saw/square/S&H; output 0–1).

### Shapers and effects (first-order ADAA on fx_fold, fx_sat, fx_shape)
- `fx_fold(in, amount)` · no slot · both — sine wavefolder: sin(in · (1 + 8·amount)).
- `fx_sat(in, drive)` · no slot · both — tanh saturation, drive 0-1 (gain 1-10), level-compensated.
- `fx_shape(in, drive, type)` · no slot · both — multi-curve waveshaper (`type` 0–1 selects curve: hard clip, tube, half-rect, full-rect, Chebyshev-3, diode foldback).
- `fx_crush(slot, in, bits, rate)` · 2 slots · both — bits 0-1 (0 clean, 1 ≈ 2 bits); rate 0-1 (1 full rate, 0 ≈ 1 kHz sample-and-hold).
- `fx_comb(slot, in, hz, feedback)` · 2 slots · both — comb tuned to hz (min 10 Hz), feedback 0-0.95; rings at hz and its harmonics, louder with feedback.
- `fx_ott(slot, in, depth)` · 2 slots · both — 3-band (120 Hz / 2.5 kHz) upward + downward compressor, depth 0-1 (0 = bypass). In master use slot 0 for L, 1 for R [85].
- `fx_phaser(slot, in, pos, depth, feedback)` · 2 slots · both — 8-stage phaser; `pos` 0-1 sweeps the notches (100 Hz-5 kHz; move it with an LFO), depth 0-1 (0 = dry), feedback 0-1 [50].
- `fx_fshift(slot, in, shift)` · 1 slot · both — frequency shifter: moves every partial by the same Hz (inharmonic, metallic); `shift` 0.5 = none, toward 0/1 = down/up to 2 kHz, exponential (0.6 ≈ +6 Hz, 0.75 ≈ +60 Hz).

### Physical models (driven by any exciter signal)
- `phys_resonator(slot, exciter, hz, structure, brightness, damping)` · 1 slot · both — modal resonator; structure, brightness, damping 0-1 [190].
- `phys_string(slot, exciter, hz, brightness, damping)` · 1 slot · both — plucked string; lowest pitch ≈ srate / 1020 (about 94 Hz in voice), so not for sub basses [150].

### Chaos
- `chaos_lorenz(slot, rate, axis)` · 2 slots · both — Lorenz attractor ±1; rate 0-1 maps to 0.05 Hz-1 kHz (near 0 = slow LFO, near 1 = audio); axis 0, 1 or 2.
- `chaos_duffing(slot, hz, chaos)` · 2 slots · both — forced Duffing oscillator ±1 moving at about hz (max ≈ 0.076·srate); chaos 0-1 (0.2 periodic, near 1 chaotic).

### Utilities
- `util_safe_fb(slot, x)` · 4 slots · both — DC block + slew limit + tanh. Every feedback path must pass through it: `fb1 = util_safe_fb(0, y);`.
- `util_xover_lo(slot, in)` · 2 slots · both — Linkwitz-Riley 4th-order crossover at 100 Hz; returns the low band.
- `util_xover_hi(slot)` · 2 slots · both — the high band of that slot's last util_xover_lo call (call _lo first).
- `util_bytebeat(t, mode)` · no slot · both — bytebeat formula of an integer counter t, mode 0-3; output ±1.

### Master bus
- `bus_sat(in, drive)` · no slot · both — same curve as fx_sat; call once per channel.
- `bus_mono_low(l, r)` · no slot · master — sums below 100 Hz to mono; returns the new L.
- `bus_mono_low_r()` · no slot · master — the new R from the last bus_mono_low.
- `bus_chorus(l, r, depth, mix)` · no slot · master — stereo chorus; returns L.
- `bus_chorus_r()` · no slot · master — R from the last bus_chorus.
- `bus_reverb(l, r, size, mix)` · no slot · master — small FDN reverb; returns L.
- `bus_reverb_r()` · no slot · master — R from the last bus_reverb.
- `bus_delay(l, r, beats, feedback, mix)` · no slot · master — tempo-synced ping-pong delay; beats 0.0625-4 (0.75 = dotted 1/8), feedback 0-1, mix 0-1; echoes are band-passed 150 Hz-6 kHz, so the sub stays dry; returns L.
- `bus_delay_r()` · no slot · master — R from the last bus_delay.
- `bus_width(l, r, amount)` · no slot · master — stereo width above 150 Hz (0 = mono, 0.5 = unchanged, 1 = wide); the low end is untouched. inl == inr, so it widens only what a stereo effect before it made (bus_chorus, bus_delay, bus_reverb); returns L.
- `bus_width_r()` · no slot · master — R from the last bus_width.

The master has its own slots: voice functions called there are separate instances from the voices' ones.

## EEL2 built-ins
`sin(x)` `cos(x)` `tan(x)` `asin(x)` `acos(x)` `atan(x)` `atan2(y, x)` `sqrt(x)` `sqr(x)` `invsqrt(x)` `pow(x, y)` `exp(x)` `log(x)` `log10(x)` `abs(x)` `sign(x)` `min(a, b)` `max(a, b)` `floor(x)` `ceil(x)` `rand(x)` (0..x) `sigmoid(x, k)` `band(a, b)` `bor(a, b)` `loop(n, code)` `while(code)` `exec2(a, b)` `exec3(a, b, c)` `memset(dest, value, n)` `memcpy(dest, src, n)` `mem_set_values(i, v1, v2)` `mem_get_values(i, v1, v2)` `mem_multiply_sum(a, b, n)`.
Nothing else exists: no tanh (use fx_sat), clamp (use min/max), round (floor(x + 0.5)), fmod (x - floor(x / y) * y) or lerp; delays in voice code are built in mem[] (bus_delay is master only).

## Limits
- `mem[]`: 65,536 slots per VM (each voice and the master have their own), zeroed on load; use it for delay lines and tables (`mem[i]`, `buf = 1000; buf[i]`).
- `loop`/`while`: at most 1024 iterations each, nested at most 2 deep (a loop in a function counts where it is called).
- Each code section ≤ 32 KB, the whole patch ≤ 256 KB.
- Slots: a slot is one instance with its own state. Give every call site its own slot (`osc_saw(0, f)` and `osc_saw(1, f * 1.01)`); two calls on the same function and slot share and corrupt state. Pools are per function: `osc_saw(0, ...)` and `osc_sine(0, ...)` are independent. Slot numbers are literal integers within the pool.
