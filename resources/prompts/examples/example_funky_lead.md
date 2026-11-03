category: lead
keywords: lead, solo, melody, funk, funky, sync, wah, phaser, delay, mono, glide
prompt: funky digital sync lead with a cutoff wah, phaser and tempo delay
---
```json
{
  "name": "Funky Sync Lead",
  "voiceMode": "mono",
  "macros": [
    {
      "label": "Wah",
      "default": 0.56
    },
    {
      "label": "Drive",
      "default": 0.426667
    },
    {
      "label": "Reso",
      "default": 0.0
    },
    {
      "label": "Groove",
      "default": 0.986667
    },
    {
      "label": "Sync",
      "default": 1.0
    },
    {
      "label": "Phaser",
      "default": 0.0
    },
    {
      "label": "Space",
      "default": 1.0
    },
    {
      "label": "Squash",
      "default": 0.4
    }
  ],
  "graph": {
    "edges": [
      [
        "osc_sync:0",
        "flt_lp:0"
      ],
      [
        "flt_lp:0",
        "fx_phaser:0"
      ],
      [
        "fx_phaser:0",
        "fx_ott:0"
      ],
      [
        "fx_ott:0",
        "out"
      ],
      [
        "out",
        "bus_delay"
      ]
    ]
  }
}
```
```eel init
```
```eel voice
// pitch vibrato with mod wheel control
vib = osc_sine(0, 5.5) * (0.003 + mw * 0.008);
f = freq * (1 + vib);
// hard sync oscillator with slave ratio sweep, layered with octave saw body
sy = osc_sync(0, f, 0.12 + m5 * 0.25, 0.2 + m5 * 0.6);
body = osc_saw(0, f * 0.5) * 0.35;
x = sy * 0.7 + body * 0.3;
env = env_adsr(0, gate, 0.01, 0.3, 0.8, 0.35);
fenv = env_adsr(1, gate, 0.005, 0.28, 0, 0.3);
// tempo-synced groove lfo sweeps cutoff for wah effect
per = m4 < 0.33 ? 1 : m4 < 0.66 ? 0.5 : 0.25;
lfo = mod_lfo(0, per, 0.25 + m4 * 0.25) - 0.5;
cut = 0.15 + m1 * 0.3 + fenv * (0.15 + m1 * 0.45) + lfo * m4 * 0.3;
cut = min(max(cut, 0.05), 0.95);
x = flt_lp(0, x, cut, m3 * 0.8);
x = fx_sat(x, 0.2 + m2 * 0.6);
// phaser adds swooshing harmonics, ott gives compressed presence
x = fx_phaser(0, x, min(max(0.2 + m1 * 0.3 + fenv * 0.3 + lfo * m4 * 0.4, 0), 1), 0.2 + m6 * 0.5, 0.4);
x = fx_ott(0, x, 0.15 + m8 * 0.3);
out = x * env * (0.6 + vel * 0.4);
```
```eel master
// mono bass frequencies retained while delay creates stereo slapback
ml = bus_mono_low(inl, inr);
mr = bus_mono_low_r();
d = bus_delay(ml, mr, 0.75, 0.35, 0.05 + m7 * 0.2, 0.35);
dr = bus_delay_r();
outl = bus_width(d, dr, 0.65);
outr = bus_width_r();
```
