# FM wonk with pitch envelopes
keywords: wonk, wonky, halftime, fm, donk, dive, drop, zap, laser, pitch, bend, squelch, bouncy, goofy, 140
Use `osc_fm(slot, hz, ratio, index, feedback)` (`ratio` 0.2 = 1×, 0.4 = 2×, 0.52 = 3×, 0.6 = 4×): each note dives in from above while the FM index decays, so every hit talks. Keep a clean sine sub on `f`.
```eel
// init: pe = 1; ie = 1; pk = exp(-1 / (0.05 * srate)); ik = exp(-1 / (0.15 * srate));
pe *= pk; ie *= ik;
f = freq * exp(pe * m2 * 12 * 0.0577623);   // dive from up to +12 semitones
r = m4 < 0.33 ? 0.2 : m4 < 0.66 ? 0.4 : 0.6; // 1x, 2x, 4x harmonic ratio
idx = min(1, 0.1 + ie * (0.2 + m1 * 0.6) + mod_lfo(0, 0.5, 0) * m3 * 0.35);
y = osc_fm(0, f, r, idx, m5 * 0.5);
y = fx_sat(flt_hp(0, y, 0.27, 0) * 1.5, 0.3 + m1 * 0.4);
out = (osc_sine(0, f) * 0.5 + y * 0.5) * env_adsr(0, gate, 0.001, 0.3, 0.8, 0.3);
```
