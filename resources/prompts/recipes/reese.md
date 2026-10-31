# Reese
keywords: reese, dnb, drum and bass, jungle, rolling, detune, detuned, beating, two saws, saws, dark, liquid, neuro
A supersaw and one saw detuned the other way beat; one envelope opens the filter and shapes the amp; the last sample nudges the pitch (snarl). Only the mids are driven; their beating low end is high-passed away under a clean sine, so the low end never phases.
```eel
// init: fb1 = 0;
det = 1 + (m2 - 0.5) * 0.02;
a = osc_supersaw(0, freq * det * (1 + fb1 * m1 * 0.02), m2, 0.65);
b = osc_saw(0, freq * (2 - det));
env = env_adsr(0, gate, 0.01, 0.35, 0.8, 0.35);
y = flt_lp(0, a * 0.7 + b * 0.3, min(0.95, 0.15 + m3 * 0.5 + env * 0.25), 0.4);
y = fx_fold(fx_sat(y, 0.3 + m1 * 0.6), m1 * 0.35);
fb1 = util_safe_fb(0, y);
y = flt_hp(1, y, 0.27, 0);
out = (osc_sine(0, freq) * 0.5 + y * 0.5) * env;
```
