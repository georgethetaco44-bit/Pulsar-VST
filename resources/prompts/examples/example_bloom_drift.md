category: pad
keywords: pad, pads, strings, atmosphere, lush, evolving, drift, bloom, ambient, chord, poly, slow attack
prompt: lush evolving pad, detuned saws and a soft triangle, chorus, wide reverb, slow attack
---
```json
{
  "name": "Bloom Drift",
  "voiceMode": "poly",
  "macros": [
    {
      "label": "Evolve",
      "default": 0.0
    },
    {
      "label": "Detune",
      "default": 0.0
    },
    {
      "label": "Cutoff",
      "default": 0.56
    },
    {
      "label": "Reso",
      "default": 0.0
    },
    {
      "label": "Glow",
      "default": 1.0
    },
    {
      "label": "Attack",
      "default": 1.0
    },
    {
      "label": "Space",
      "default": 1.0
    },
    {
      "label": "Width",
      "default": 0.933333
    }
  ],
  "graph": {
    "edges": [
      [
        "osc_supersaw:0",
        "flt_lp:0"
      ],
      [
        "osc_tri:0",
        "flt_lp:0"
      ],
      [
        "flt_lp:0",
        "bus_reverb"
      ],
      [
        "bus_reverb",
        "out"
      ]
    ]
  }
}
```
```eel init
dc = 0;
```
```eel voice
// slow lfo creates organic pitch and filter movement
lfo = mod_lfo(0, 4, 0.2 + m1 * 0.5);
env = env_adsr(0, gate, 0.55 + m6 * 0.25, 0.5, 0.9, 0.65);
// supersaw with gentle detune blend plus warm triangle core
a = osc_supersaw(0, freq * (1 + (lfo - 0.5) * 0.007), 0.1 + m2 * 0.8, 0.7);
b = osc_tri(0, freq * 1.005);
cut = 0.2 + m3 * 0.4 + (lfo - 0.5) * (0.05 + m1 * 0.35);
x = flt_lp(0, a * 0.6 + b * 0.4, min(0.9, max(0.05, cut)), m4 * 0.6);
x = fx_sat(x, 0.15 + m5 * 0.4);
dc += (x - dc) * 20 / srate;
out = (x - dc) * env * (0.5 + 0.3 * vel);
```
```eel master
// chorus widens stereo image, reverb creates spacious hall, bus_width spreads it wide
sl = flt_tilt(0, inl, 0.4 + m1 * 0.2);
sr = flt_tilt(1, inr, 0.4 + m1 * 0.2);
cl = bus_chorus(sl, sr, 0.25 + m1 * 0.4, 0.25 + m5 * 0.25);
cr = bus_chorus_r();
d = bus_reverb(cl, cr, 0.65, 0.12 + m7 * 0.3);
dr = bus_reverb_r();
outl = bus_width(d, dr, 0.5 + m8 * 0.5);
outr = bus_width_r();
```
