category: texture
keywords: pad, texture, drone, ambient, atmosphere, evolving, chaos, chaotic, fm, feedback, scream, screech, howl, metallic, alien, weird, cinematic, eerie, dark
prompt: two oscillators that frequency-modulate each other through filters, one sample apart; the coupling goes from a clean tone to a screaming chaotic howl
---
```json
{
  "name": "Coupled Scream",
  "voiceMode": "poly",
  "macros": [
    {"label": "Coupling", "default": 0.3},
    {"label": "Loop Tone", "default": 0.45},
    {"label": "Loop Res", "default": 0.3},
    {"label": "Ratio", "default": 0.4},
    {"label": "Bite", "default": 0.4},
    {"label": "Drive", "default": 0.3},
    {"label": "Attack", "default": 0.1},
    {"label": "Space", "default": 0.3}
  ],
  "graph": {"edges": [["osc_a", "flt_lp:0"], ["flt_lp:0", "osc_b"], ["osc_b", "flt_lp:1"], ["flt_lp:1", "osc_a"], ["osc_a", "out"], ["out", "bus_reverb"]]}
}
```
```eel init
fb1 = 0; fb2 = 0;
```
```eel voice
// partner ratio: 1:1 beating, 3:2, 2:1, 3:1
r = m4 < 0.25 ? 1.003 : m4 < 0.5 ? 1.5 : m4 < 0.75 ? 2.002 : 3.001;
env = env_adsr(0, gate, 0.003 + m7 * 0.6, 0.5, 0.85, 0.45);
// Coupling is the hero (chaos near 2 rad), plus a burst per note (Bite)
cpl = m1 * sqrt(m1) * 3 + env_adsr(1, gate, 0.001, 0.14, 0, 0.1) * m5 * 2.5;
// two phase oscillators, each modulated by the other's last filtered sample
ph1 += freq / srate; ph1 -= floor(ph1);
ph2 += freq * r / srate; ph2 -= floor(ph2);
y1 = sin(2 * $pi * ph1 + cpl * fb2);
y2 = sin(2 * $pi * ph2 + cpl * fb1 * 1.3);
tone = 0.3 + m2 * 0.65;
fb1 = util_safe_fb(0, flt_lp(0, y1, tone, m3 * 0.85));
fb2 = util_safe_fb(1, flt_lp(1, y2, tone, m3 * 0.85));
// DC trap: heavy phase modulation drifts
y = y1 * 0.6 + y2 * 0.4;
dc += (y - dc) * 20 / srate;
out = (y - dc) * env * (0.6 + 0.4 * vel) * 0.5;
```
```eel master
// amp: drive, a cab roll-off, then chorus width and a room
x = fx_sat(inl * (1 + m6 * 4), 0.2 + m6 * 0.6);
x = flt_lp(0, x, 0.84, 0.2);
cl = bus_chorus(x, x, 0.3, 0.3);
cr = bus_chorus_r();
outl = bus_reverb(cl, cr, 0.5, m8 * 0.4);
outr = bus_reverb_r();
```
