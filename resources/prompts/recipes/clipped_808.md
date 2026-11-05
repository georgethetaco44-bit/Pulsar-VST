# Distorted / clipped 808
category: bass-fm
keywords: 808, distorted 808, clipped, clip, trap, rage, hyperpop, slide, slides, glide, boom, overdriven
A trap/rage 808 uses a single sine wave with a fast pitch drop on attack (+12 semitones falling in 30–60 ms) and a long boom decay. Because the 808 has no separate mids, distorting the whole source does not violate the nonlinear sub rule. Run through heavy saturation into a hard clip (`fx_shape` type 0) on a Drive macro, then lowpass to tame harsh fizz, mixing a little clean sine back for fundamental weight. Slides glide on log pitch in voice code.
```eel
target_p = log(max(freq, 20)) / log(2);
lp += (target_p - lp) * (0.003 + m2 * 0.02);
pdrop = env_adsr(1, gate, 0.001, 0.045, 0.0, 0.05);
f = pow(2, lp + pdrop * 1.0);
sine = osc_sine(0, f);
dist = fx_shape(fx_sat(sine, 0.5 + m1 * 0.4), 0.7, 0);
dist = flt_lp(0, dist, 0.55 + m3 * 0.25, 0.1);
env = env_adsr(0, gate, 0.002, 0.8 + m4 * 0.8, 0.1, 0.35);
out = (sine * 0.35 + dist * 0.65) * env;
```
