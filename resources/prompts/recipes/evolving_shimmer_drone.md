# Evolving Shimmer Drone
category: ambient
keywords: ambient, drone, evolving, shimmer, bloom, soundscape, cinematic, partials, slow
A per-note timer resets on note-on and slowly blooms over tens of seconds. Wavetable scanning, high-octave sine shimmer partials, and feedback delay build vast cinematic textures.
```eel
// init: t = 0;
t += 1 / srate;
bloom = min(t / 20, 1);
pos = min(max(0.1 + bloom * (0.3 + m1 * 0.5), 0), 1);
a = osc_wt(0, freq, 7, pos);
shim = osc_sine(1, freq * 4) * 0.15 * (m2 * 0.7 + bloom * 0.3);
x = flt_formant(0, a, 0.2 + bloom * 0.4, 0.6);
env = env_adsr(0, gate, 0.8 + m3 * 0.5, 0.5, 0.9, 0.8);
out = (x * 0.7 + shim) * env * 0.5;
// master
r = bus_reverb(inl, inr, 0.75, 0.2 + m4 * 0.3);
rr = bus_reverb_r();
outl = r;
outr = rr;
```
