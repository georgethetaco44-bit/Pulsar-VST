category: ambient
keywords: ambient, drone, evolving, blooms, soundscape, cinematic, atmosphere, shimmer, pulsing
prompt: evolving ambient drone that blooms over 30 seconds with shimmer
---
```json
{
  "name": "Bloom Pulse",
  "voiceMode": "poly",
  "macros": [
    {
      "label": "Bloom",
      "default": 1.0
    },
    {
      "label": "Shimmer",
      "default": 0.96
    },
    {
      "label": "Vowel",
      "default": 0.013333
    },
    {
      "label": "Drift",
      "default": 0.0
    },
    {
      "label": "Glow",
      "default": 0.3
    },
    {
      "label": "Motion",
      "default": 0.246667
    },
    {
      "label": "Attack",
      "default": 0.6
    },
    {
      "label": "Space",
      "default": 0.5
    }
  ],
  "graph": {
    "edges": [
      [
        "osc_wt:0",
        "flt_formant:0"
      ],
      [
        "flt_formant:0",
        "fx_sat"
      ],
      [
        "osc_sine:1",
        "out"
      ],
      [
        "flt_formant:0",
        "out"
      ],
      [
        "out",
        "bus_reverb"
      ]
    ]
  }
}
```
```eel init
g = 1;
bloom = 0;
dt = 1 / (30 * srate);
```
```eel voice
// per-note timer resets on init and counts up to 30 seconds for the bloom curve
bloom = min(bloom + dt, 1);
per = m6 < 0.33 ? 2.0 : m6 < 0.66 ? 1.0 : 0.5;
invPer = m6 < 0.33 ? 0.5 : m6 < 0.66 ? 1.0 : 2.0;
sq = mod_lfo(0, per, 0.3 + m6 * 0.4);
seq = sq - 0.5;
// wavetable position and formant vowel drift slowly over time
pos = 0.15 + bloom * (0.25 + m1 * 0.5) + m4 * 0.15 + seq * (0.1 + m1 * 0.35);
a = osc_wt(0, freq, 7, pos);
vw = m3 * 0.6 + bloom * (0.2 + m1 * 0.4) + seq * (0.25 + m1 * 0.55);
x = flt_formant(0, a, vw, 0.65);
x = fx_sat(x, 0.15 + m5 * 0.6 + m1 * 0.3);
// high-octave sine shimmer partial blooms into the sound
shimAmt = m2 * 0.8 + (m1 + bloom) * 0.5;
s1 = osc_sine(0, freq * 4) * shimAmt * (0.1 + 0.12 * sq);
// soft tempo gate pulse
bp = beat * invPer;
ph = bp - floor(bp);
tgt = ph < 0.65 ? 1 : 1 - m1 * 0.75;
g += (tgt - g) * 0.04;
env = env_adsr(0, gate, 0.55 + m7 * 0.3, 0.5, 0.9, 0.65);
out = (x * 0.75 + s1) * env * g;
```
```eel master
// chorus and expansive reverb create a wide, immersive soundscape
c = bus_chorus(inl, inr, 0.4, 0.25);
cr = bus_chorus_r();
r = bus_reverb(c, cr, 0.72, 0.1 + m8 * 0.3);
rr = bus_reverb_r();
outl = bus_width(r, rr, 0.8);
outr = bus_width_r();
```
