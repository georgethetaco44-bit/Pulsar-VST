# Tearout screech
category: bass-fm
keywords: tearout, screech, scream, screaming, yell, metallic, brutal, aggressive, nasty, harsh, piercing, howl
Build the screech on the mid band only, pitched up 12 or 19 semitones above the clean sub. A snappy pitch envelope drops 3–5 semitones on attack to make it scream. Run a safe FM feedback loop through a resonant `fx_comb` tuned to double the mid pitch, followed by Chebyshev waveshaping, a peak boost around 2–4 kHz, and `fx_ott`. Scale m1 so 1.0 remains usable and never turns to white noise.
```eel
penv = env_adsr(1, gate, 0.001, 0.12, 0.0, 0.05);
mid_f = freq * 2 * pow(2, (penv * (4 + m2 * 3)) / 12);
fb = util_safe_fb(0, prev);
mid = osc_fm(0, mid_f, 0.25, fb * (0.8 + m1 * 0.7), 0.0);
prev = mid;
mid = fx_comb(0, mid, mid_f * 2, 0.4 + m1 * 0.45);
mid = fx_shape(mid, 0.5 + m1 * 0.4, 0.8);
mid = flt_peak(0, mid, 0.65, 0.7, 6.0);
mid = flt_hp(0, fx_ott(0, mid, 0.7), 0.27, 0);
out = (osc_sine(0, freq) * 0.5 + mid * 0.5) * env_adsr(0, gate, 0.002, 0.25, 0.7, 0.25);
```
