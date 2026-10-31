category: pluck
keywords: pluck, key, keys, piano, bell, mallet, stab, chord, arp, harp, guitar, marimba, organ, rhodes, lead, house, garage, trance, poly
prompt: short bright pluck with a decaying filter and a little chorus
---
```json
{
  "name": "Bright Pluck",
  "voiceMode": "poly",
  "macros": [
    {"label": "Bright", "default": 0.45},
    {"label": "Decay", "default": 0.4},
    {"label": "Res", "default": 0.3},
    {"label": "Body", "default": 0.5},
    {"label": "Detune", "default": 0.3},
    {"label": "Chorus", "default": 0.4},
    {"label": "Space", "default": 0.2},
    {"label": "Squash", "default": 0.5}
  ],
  "graph": {"edges": [["osc_saw:0", "flt_lp:0"], ["osc_square:0", "flt_lp:0"], ["env_adsr:1", "flt_lp:0"], ["flt_lp:0", "out"], ["out", "bus_chorus"], ["bus_chorus", "bus_reverb"]]}
}
```
```eel init
```
```eel voice
// zero-sustain amp and filter envelopes make the pluck
env = env_adsr(0, gate, 0, 0.5 + m2 * 0.2, 0, 0.4);
fenv = env_adsr(1, gate, 0, 0.42 + m2 * 0.2, 0, 0.35);
// saw + slightly detuned square, a sine for body
x = osc_saw(0, freq) * 0.6 + osc_square(0, freq * (1 + m5 * 0.01), 0.5) * 0.4;
x += osc_sine(0, freq) * m4 * 0.6;
x = flt_lp(0, x, 0.15 + m1 * 0.35 + fenv * 0.45, m3 * 0.8);
out = fx_sat(x, 0.25) * env * vel;
```
```eel master
// width, a room, then glue
cl = bus_chorus(inl, inr, 0.3, m6 * 0.5);
cr = bus_chorus_r();
rl = bus_reverb(cl, cr, 0.5, m7 * 0.4);
rr = bus_reverb_r();
outl = bus_sat(fx_ott(0, rl, m8), 0.2);
outr = bus_sat(fx_ott(1, rr, m8), 0.2);
```
