category: chords
keywords: chord, chords, one finger, minor 9, maj7, stab chord, lofi, house chord, poly, pad
prompt: one-finger lush minor 9 chord pad, warm, slow attack
---
```json
{
  "name": "Minor9 Lush",
  "voiceMode": "poly",
  "macros": [
    {
      "label": "Bright",
      "default": 0.486598
    },
    {
      "label": "Motion",
      "default": 0.529897
    },
    {
      "label": "Attack",
      "default": 0.127835
    },
    {
      "label": "Rate",
      "default": 0.931959
    },
    {
      "label": "Fifth",
      "default": 1.0
    },
    {
      "label": "Chorus",
      "default": 0.0
    },
    {
      "label": "Space",
      "default": 1.0
    },
    {
      "label": "Width",
      "default": 0.0
    }
  ],
  "graph": {
    "edges": [
      [
        "osc_saw:0",
        "flt_lp:0"
      ],
      [
        "flt_lp:0",
        "fx_sat"
      ],
      [
        "fx_sat",
        "out"
      ],
      [
        "out",
        "bus_chorus"
      ],
      [
        "bus_chorus",
        "bus_reverb"
      ]
    ]
  }
}
```
```eel init
```
```eel voice
// one-finger lush minor 9 chord voices: root, minor 3rd (1.1892), 5th (1.4983), minor 7th (1.7818), 9th (2.2449)
per = m4 < 0.33 ? 2 : m4 < 0.66 ? 1 : 0.5;
lfo = mod_lfo(0, per, 0.15);
sw = (lfo - 0.5) * 2;
env = env_adsr(0, gate, 0.62 + m3 * 0.2, 0.4, 0.85, 0.65);
f3 = freq * 1.189207;
f7 = freq * 1.781797;
f9 = freq * 2.244894;
f5 = freq * 1.498307;
x = osc_saw(0, freq) * 0.3 + osc_saw(1, f3) * 0.25 + osc_saw(2, f7) * 0.25;
x += osc_sine(0, f9) * 0.22;
x += osc_sine(1, f5) * m5 * 0.25;
x *= 0.5;
cut = 0.22 + m1 * 0.3 + sw * m2 * 0.12 + env * 0.08;
cut = min(max(cut, 0.05), 0.85);
y = flt_lp(0, x, cut, 0.25);
y = fx_sat(y, 0.25);
out = y * env * 0.7;
```
```eel master
// ott glue, chorus, and lush reverb with stereo width
gl = fx_ott(0, inl, 0.3);
gr = fx_ott(1, inr, 0.3);
cl = bus_chorus(gl, gr, 0.4, 0.15 + m6 * 0.45);
cr = bus_chorus_r();
rl = bus_reverb(cl, cr, 0.65, 0.1 + m7 * 0.3);
rr = bus_reverb_r();
outl = bus_width(rl, rr, m8);
outr = bus_width_r();
```
