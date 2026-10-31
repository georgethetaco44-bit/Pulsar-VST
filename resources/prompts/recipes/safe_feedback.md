# One-sample feedback
keywords: feedback, fm, scream, screech, screechy, howl, snarl, chaos, chaotic, unstable, nasty, aggressive, neuro, feedback loop
The output of a folder, filter or resonator feeds pitch, FM index or cutoff on the next sample. Always through util_safe_fb, with its state reset in init. In a bass, the loop lives on the mid band and the sub is added after it.
```eel
// init: fb1 = 0;
x = osc_sine(0, freq * (1 + fb1 * m1 * 4));
y = fx_fold(flt_lp(0, x, 0.4 + m2 * 0.5, m3 * 0.8), 0.2 + m1 * 0.6);
fb1 = util_safe_fb(0, y);
mid = fx_sat(flt_hp(1, y, 0.27, 0) * 1.5, 0.4);
out = (osc_sine(1, freq) * 0.5 + mid * 0.5) * env_adsr(0, gate, 0.005, 0.3, 0.85, 0.3);
```
