# Vibrato Lead
category: lead
keywords: lead, solo, melody, mono, glide, vibrato, mod wheel, pitch, express
An expressive mono lead with portamento glide. The mod wheel (mw) scales pitch vibrato depth, while an envelope opens the filter for biting articulation.
```eel
vib = osc_sine(0, 5.5) * (0.002 + mw * 0.012 * (0.5 + m1 * 0.5));
f = freq * (1 + vib);
src = osc_saw(0, f) * 0.6 + osc_square(0, f * 1.002, 0.5) * 0.4;
fenv = env_adsr(1, gate, 0.005, 0.25, 0.0, 0.25);
env = env_adsr(0, gate, 0.01, 0.35, 0.85, 0.3);
cut = min(0.95, 0.2 + m2 * 0.4 + fenv * (0.2 + m3 * 0.4));
x = flt_lp(0, src, cut, m4 * 0.6);
out = fx_sat(x, 0.2 + m5 * 0.3) * env * (0.6 + vel * 0.4);
// master
d = bus_delay(inl, inr, 0.75, 0.35, 0.15 + m6 * 0.2, 0.3);
dr = bus_delay_r();
outl = d;
outr = dr;
```
