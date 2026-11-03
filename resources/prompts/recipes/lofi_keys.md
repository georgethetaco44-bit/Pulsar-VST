# Lo-Fi Keys
category: chords
keywords: lofi, keys, piano, rhodes, vintage, wow, flutter, crackle, tape, warm
Mellow triangle and sine waves with slow pitch wow and subtle noise crackle emulate aged electric pianos and tape recordings. Soft lowpass filtering rolls off digital brightness.
```eel
// slow pitch wow emulation
wow = osc_sine(0, 0.8) * 0.003 * m1;
f = freq * (1 + wow);
src = osc_tri(0, f) * 0.6 + osc_sine(1, f * 2) * 0.25 * m2;
crackle = noise() * 0.02 * m3;
env = env_adsr(0, gate, 0.01, 0.6 + m4 * 0.4, 0.3, 0.4);
cut = min(0.85, 0.2 + m5 * 0.35 + vel * 0.15);
x = flt_lp(0, src + crackle, cut, 0.1);
out = fx_sat(x, 0.2) * env * (0.5 + vel * 0.5);
```
