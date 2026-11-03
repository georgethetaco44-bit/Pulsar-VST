# Clean FM Wub
category: bass-fm
keywords: clean, fm, sine, wub, wobble, ratio, pure, dubstep, 1:2, simple
A 1:2 modulator-to-carrier FM pair delivers round, vocal dubstep wubs with pure harmonic overtones. A tempo-synced LFO sweeps the modulation index smoothly.
```eel
per = m2 < 0.33 ? 1 : m2 < 0.66 ? 0.5 : 0.25;
lfo = mod_lfo(0, per, 0);
wub = (lfo - 0.5) * 2 * m1;
fmMod = min(max(m3 * 0.6 + wub * 0.3, 0), 1);
mid = osc_fm(0, freq, 2.0, fmMod, 0);
mid = flt_hp(0, mid, 0.25, 0);
cut = min(max(0.2 + m4 * 0.4 + wub * 0.25, 0.05), 0.95);
mid = flt_lp(0, mid, cut, 0.3);
sub = osc_sine(0, freq) * m5 * 0.8;
env = env_adsr(0, gate, 0.01, 0.3, 0.85, 0.3);
out = (sub * 0.7 + mid * 0.5) * env;
```
