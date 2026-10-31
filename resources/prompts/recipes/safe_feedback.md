# One-sample feedback
keywords: feedback, fm, scream, screech, howl, growl, snarl, chaos, chaotic, unstable, metallic, aggressive, neuro, feedback loop
The output of a folder, filter or resonator feeds pitch, FM index or cutoff on the next sample. Always through util_safe_fb, and reset the state in init.
```eel
// init: fb1 = 0;
x = osc_sine(0, freq * (1 + fb1 * m1 * 4));
y = fx_fold(flt_lp(0, x, m2, m3), m4);
fb1 = util_safe_fb(0, y);
out = y * env_adsr(0, gate, 0.01, 0.3, 0.8, 0.3);
```
