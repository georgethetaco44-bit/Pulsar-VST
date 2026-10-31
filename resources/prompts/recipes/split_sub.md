# Split-brain sub bass
keywords: bass, sub, reese, neuro, dubstep, dnb, halftime, growl, wobble, 808, low end, mid bass, riddim, foghorn
A clean, phase-stable sine sub under ~110 Hz; all the surgery happens on the band above. In voice, high-pass the aggressive layer (or split it with util_xover_lo/hi) and add a plain osc_sine underneath.
```eel
src = osc_saw(0, freq);
lo = util_xover_lo(0, src);
hi = util_xover_hi(0);
hi = fx_fold(flt_bp(0, hi, m1, m2), m3);
out = (osc_sine(0, freq) * 0.9 + hi * m4) * env_adsr(0, gate, 0.005, 0.3, 0.9, 0.3);
```
