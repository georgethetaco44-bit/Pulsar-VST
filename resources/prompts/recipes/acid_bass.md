# Acid Squelch
category: bass-square
keywords: acid, 303, squelch, resonance, resonant, accent, filter env, snappy, squelchy
Classic acid lines rely on a resonant lowpass filter swept by a snappy, decaying envelope. A single saw or square into a ladder filter with high resonance screams when driven.
```eel
fenv = env_adsr(1, gate, 0.002, 0.18 + m1 * 0.2, 0.0, 0.15);
env = env_adsr(0, gate, 0.005, 0.35, 0.7, 0.25);
src = osc_saw(0, freq) * 0.7 + osc_square(0, freq, 0.5) * 0.3 * m2;
cut = min(0.95, max(0.05, 0.1 + m3 * 0.35 + fenv * (0.3 + m4 * 0.4)));
res = min(0.95, 0.4 + m5 * 0.5);
x = flt_ladder(0, src, cut, res);
x = fx_sat(x, 0.25 + m6 * 0.5);
out = x * env;
```
