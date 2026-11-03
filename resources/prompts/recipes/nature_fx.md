# Nature FX
category: fx
keywords: nature, wind, rain, ocean, waves, sea, water, surf, crash, organic, ambient
Slowly modulated bandpass noise mimics wind swells, rushing streams, and ocean waves crashing on a shore. Stereo width and deep reverb complete the outdoor environment.
```eel
// slow random swells from multi-rate lfos
lfo1 = mod_lfo(0, 8, 0);
lfo2 = mod_lfo(1, 13, 0.3);
swell = (lfo1 * 0.6 + lfo2 * 0.4);
n = noise();
cut = min(0.85, max(0.08, 0.15 + swell * (0.3 + m1 * 0.4)));
x = flt_bp(0, n, cut, 0.3 + m2 * 0.4);
out = fx_sat(x, 0.15) * (0.4 + swell * 0.6);
// master
r = bus_reverb(inl, inr, 0.8, 0.25 + m3 * 0.3);
rr = bus_reverb_r();
outl = bus_width(r, rr, 0.7);
outr = bus_width_r();
```
