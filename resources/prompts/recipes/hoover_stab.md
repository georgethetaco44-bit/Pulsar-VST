# Hoover Stab
category: lead
keywords: hoover, stab, rave, mentasm, detuned, pitch bend, pitch dip, trance, classic
The classic rave hoover stacks detuned pulse and saw waves with a characteristic pitch dip on note attack. Chorus and distortion glue the dense swarm of beating harmonics.
```eel
pdrop = env_adsr(1, gate, 0.002, 0.12, 0.0, 0.2);
f = freq * (1 - pdrop * 0.25 * m1);
a = osc_saw(0, f * 0.992) + osc_saw(1, f * 1.008);
b = osc_square(0, f * 0.985, 0.3) + osc_square(1, f * 1.015, 0.7);
env = env_adsr(0, gate, 0.01, 0.4, 0.8, 0.3);
x = flt_lp(0, (a + b) * 0.3, min(0.95, 0.3 + m2 * 0.5), 0.3);
out = fx_sat(x, 0.3 + m3 * 0.4) * env;
// master
c = bus_chorus(inl, inr, 0.4, 0.35);
cr = bus_chorus_r();
outl = c;
outr = cr;
```
