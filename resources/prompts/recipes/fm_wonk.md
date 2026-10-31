# FM wonk with pitch envelopes
keywords: wonk, wonky, halftime, fm, donk, dive, drop, zap, laser, pitch, bend, squelch, bouncy, goofy, 140
2-op FM in phase math: each note dives in from above while the FM index decays, so every hit talks. Decays are set in init: exp(-1 / (seconds * srate)). The sub is a sine on the carrier's phase: it follows the dive and stays one clean partial.
```eel
// init: pe = 1; ie = 1; pk = exp(-1 / (0.05 * srate)); ik = exp(-1 / (0.15 * srate));
pe *= pk; ie *= ik;
f = freq * exp(pe * m2 * 12 * 0.0577623);   // dive from up to +12 semitones
ph += f / srate; ph -= floor(ph);
idx = 0.3 + ie * (1 + m1 * 4) + m3 * (0.5 - 0.5 * cos(beat * 2 * $pi * 2)) * 2;
y = sin(2 * $pi * ph + sin(4 * $pi * ph) * idx);
y = fx_sat(flt_hp(0, y, 0.27, 0) * 1.5, 0.3 + m1 * 0.4);
out = (sin(2 * $pi * ph) * 0.5 + y * 0.5) * env_adsr(0, gate, 0.001, 0.3, 0.8, 0.3);
```
