# Lush Pad
category: pad
keywords: pad, pads, lush, warm, soft, slow attack, chorus, reverb, space, ambient
Warm detuned saws and triangles in poly mode with a slow-attack envelope create floating, lush pads. Stereo chorus and expansive hall reverb give a cinematic wash.
```eel
lfo = mod_lfo(0, 4, 0.3);
env = env_adsr(0, gate, 0.4 + m1 * 0.6, 0.6, 0.85, 0.6);
a = osc_supersaw(0, freq * (1 + (lfo - 0.5) * 0.005), 0.1 + m2 * 0.7, 0.7);
b = osc_tri(0, freq * 1.004);
cut = min(0.9, 0.15 + m3 * 0.45 + (lfo - 0.5) * 0.1);
x = flt_lp(0, a * 0.6 + b * 0.4, cut, 0.2);
out = fx_sat(x, 0.15) * env * 0.4;
// master
c = bus_chorus(inl, inr, 0.3, 0.25 + m4 * 0.25);
cr = bus_chorus_r();
r = bus_reverb(c, cr, 0.7, 0.15 + m5 * 0.35);
rr = bus_reverb_r();
outl = bus_width(r, rr, 0.7);
outr = bus_width_r();
```
