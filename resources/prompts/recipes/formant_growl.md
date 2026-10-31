# Formant growl (talking bass)
keywords: growl, talk, talking, vowel, yoi, wah, formant, vocal, mouth, nasal, tearout, dubstep, riddim, speak, yapping
Drive a rich source first, then two band-passes at vowel formants (o 450/800 Hz, a 800/1200, e 400/2300). Sweep the vowel with the hero macro and a beat LFO; convert Hz to cutoff with log(hz / 20) / 6.908.
```eel
v = min(1, m1 * 0.7 + (0.5 - 0.5 * cos(beat * 2 * $pi * 2)) * m2 * 0.5);  // vowel, 1/8 LFO
t = v < 0.5 ? v * 2 : v * 2 - 1;
f1 = v < 0.5 ? 450 + 350 * t : 800 - 400 * t;
f2 = v < 0.5 ? 800 + 400 * t : 1200 + 1100 * t;
x = fx_sat(osc_saw(0, freq) + osc_square(0, freq * 1.005, 0.5) * 0.5, 0.6);
y = flt_bp(0, x, log(f1 / 20) / 6.908, 0.6) + flt_bp(1, x, log(f2 / 20) / 6.908, 0.6) * 0.7;
y = fx_sat(flt_hp(2, y, 0.27, 0) * 2, 0.4);
out = (osc_sine(0, freq) * 0.5 + y * 0.5) * env_adsr(0, gate, 0.002, 0.3, 0.9, 0.35);
```
