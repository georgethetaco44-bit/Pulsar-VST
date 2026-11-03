# Metal Siren
category: fx
keywords: siren, riser, sweep, uplifter, pitch sweep, metal, metallic, alarm, build, fx
A rising pitch sweep driven by a ramp LFO or macro, routed through a frequency shifter or metallic comb filter for tense rave build-ups and sirens.
```eel
lfo = mod_lfo(0, 0.25, 0);
sweep = freq * (1 + lfo * (1 + m1 * 3));
src = osc_saw(0, sweep) * 0.5 + osc_square(0, sweep * 1.01, 0.5) * 0.5;
x = fx_fshift(0, src, (m2 - 0.5) * 200);
x = fx_comb(0, x, max(20, freq * 1.5), 0.3 + m3 * 0.5);
env = env_adsr(0, gate, 0.05, 0.5, 0.9, 0.4);
out = fx_sat(x, 0.3 + m4 * 0.4) * env;
// master
d = bus_delay(inl, inr, 0.75, 0.4, 0.2 + m5 * 0.3, 0.35);
dr = bus_delay_r();
outl = d;
outr = dr;
```
