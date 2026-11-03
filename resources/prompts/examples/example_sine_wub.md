category: bass-fm
keywords: fm, sine, wub, clean, pure, dubstep, bass, sub, wobble, lfo, 808
prompt: clean fm sine wave dubstep wub at a 1:2 ratio, simple lfo wobble, pure sub
---
```json
{
  "name": "Sine Wave Wub",
  "voiceMode": "mono",
  "macros": [
    {
      "label": "Wobble",
      "default": 0.24
    },
    {
      "label": "FM Index",
      "default": 0.295347
    },
    {
      "label": "Cutoff",
      "default": 0.233333
    },
    {
      "label": "Rate",
      "default": 0.5
    },
    {
      "label": "Sub",
      "default": 1.0
    },
    {
      "label": "Feedback",
      "default": 0.0
    },
    {
      "label": "Squash",
      "default": 0.0
    },
    {
      "label": "LFO Shape",
      "default": 0.0
    }
  ],
  "graph": {
    "edges": [
      [
        "osc_fm:0",
        "flt_lp:0"
      ],
      [
        "flt_lp:0",
        "flt_hp:0"
      ],
      [
        "flt_hp:0",
        "fx_ott:0"
      ],
      [
        "fx_ott:0",
        "out"
      ],
      [
        "osc_sine:0",
        "out"
      ]
    ]
  }
}
```
```eel init
```
```eel voice
// tempo-synced lfo wobbles fm depth and lowpass filter cutoff
per = m2 < 0.33 ? 1 : m2 < 0.66 ? 0.5 : 0.25;
lfo = mod_lfo(0, per, 0);
wub = (lfo - 0.5) * 2 * m1;
// 1:2 carrier-to-modulator fm produces bright harmonic sidebands
fmMod = min(max(m4 * 0.8 + wub * 0.35, 0), 1);
mid = osc_fm(0, freq, 2.0, fmMod, 0);
// highpass removes mud; lowpass follows wobble modulation
mid = flt_hp(0, mid, 0.25, 0);
cut = min(max(0.15 + m3 * 0.45 + wub * 0.3, 0.05), 0.95);
mid = flt_lp(0, mid, cut, m7 * 0.7);
mid = fx_sat(mid, 0.2);
// clean pure sine sub carries foundation below the fm growl
sub = osc_sine(0, freq) * m5 * 0.9;
env = env_adsr(0, gate, 0.01, 0.3, 0.85, 0.3);
out = (sub * 0.7 + mid * 0.5) * env;
```
```eel master
// glue compression and optional room reverb for width
gl = fx_ott(0, inl, m8 * 0.5);
gr = fx_ott(1, inr, m8 * 0.5);
r = bus_reverb(gl, gr, 0.5, m6 * 0.3);
rr = bus_reverb_r();
outl = r;
outr = rr;
```
