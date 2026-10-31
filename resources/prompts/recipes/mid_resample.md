# Staged mid-bass (resampling style)
keywords: bass, sub, mid bass, neuro, dubstep, dnb, halftime, riddim, distort, distortion, drive, resample, heavy, grind, crush, foghorn, wub, multiband, clean sub, 808, low end
Like resampling: each stage processes the last one's result, all on the mid band. The sub is a sine added at the end; nothing in the chain reads it.
```eel
src = osc_saw(0, freq) + osc_saw(1, freq * 2.003) * 0.4;
s1 = fx_sat(src, 0.3 + m1 * 0.6);                             // stage 1: drive
s2 = fx_fold(flt_lp(0, s1, 0.35 + m2 * 0.45, 0.5), m1 * 0.4);  // stage 2: filter, then fold that
mid = flt_hp(1, s2, 0.27, 0);                                  // clear 20-130 Hz (also traps the fold's DC)
mid = fx_sat(fx_ott(0, mid, 0.3 + m8 * 0.6), 0.4);             // stage 3: OTT after the drive, fixed ceiling
env = env_adsr(0, gate, 0.002, 0.3, 0.9, 0.35);
out = (osc_sine(0, freq) * 0.5 + mid * 0.5) * env;
```
