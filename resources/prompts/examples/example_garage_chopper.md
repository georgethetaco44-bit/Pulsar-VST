category: bass-square
keywords: square, pulse, pwm, riddim, garage, chopper, 8 bit, bass, sub, ladder, speed garage, wub
prompt: heavy square bass through an analog ladder filter, chopped speed garage wub, big subby low end with overtones
---
```json
{
  "name": "Garage Chopper",
  "voiceMode": "mono",
  "macros": [
    {
      "label": "Drive",
      "default": 1.0
    },
    {
      "label": "Cutoff",
      "default": 0.0
    },
    {
      "label": "Reso",
      "default": 0.544982
    },
    {
      "label": "Wobble",
      "default": 0.0
    },
    {
      "label": "Rate",
      "default": 1.0
    },
    {
      "label": "Punch",
      "default": 0.0
    },
    {
      "label": "Squash",
      "default": 1.0
    },
    {
      "label": "Sub",
      "default": 1.0
    }
  ],
  "graph": {
    "edges": [
      [
        "osc_square:0",
        "flt_ladder:0"
      ],
      [
        "osc_square:1",
        "flt_ladder:0"
      ],
      [
        "flt_ladder:0",
        "fx_sat"
      ],
      [
        "fx_sat",
        "flt_hp:0"
      ],
      [
        "flt_hp:0",
        "fx_comb:0"
      ],
      [
        "fx_comb:0",
        "fx_ott:0"
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
dc = 0;
cr = 2;
g = 1;
```
```eel voice
// wobble and mod envelope drive pulse-width and ladder cutoff
move = 0.25 + 0.75 * m1;
per = m5 < 0.33 ? 1 : m5 < 0.66 ? 0.5 : 0.25;
lfo = (mod_lfo(0, per, 0.25) - 0.5) * 2 * m4 * move;
env = env_adsr(0, gate, 0.18, 0.0, 0.845, 0.277);
penv = env_adsr(1, gate, 1.0, 0.15, 0.0, 0.35);
modenv = env_adsr(2, gate, 0.27, 0.054, 0.0, 0.3);
pw = 0.5 + lfo * 0.12;
pw = min(0.85, max(0.15, pw));
// three square oscillators (detuned pair plus sub-octave square)
src = osc_square(0, freq * 1.003, pw) * 0.55 + osc_square(1, freq * 0.997, pw) * 0.55 + osc_square(2, freq * 0.5, 0.5) * 0.35;
src += osc_tri(0, freq * 2) * penv * m6 * 0.6;
// ladder filter swept by cutoff macro, mod envelope, amp env, and wobble lfo
cut = 0.15 + (m2 + modenv * 0.323) * 0.4 + env * (0.1 + m6 * 0.2) + lfo * 0.22;
cut = min(0.9, max(0.06, cut));
f = flt_ladder(0, src, cut, 0.15 + m3 * 0.65);
s1 = fx_sat(f, 0);
y = fx_shape(s1, 0, 0.133333);
dc += (y - dc) * 20 / srate; y -= dc;
// comb resonance and tempo-synced chopper gate
mid0 = flt_hp(0, y, 0.479, 0);
st = floor(beat * 4) % 4;
tg = st == 0 ? 2 : st == 1 ? 3 : st == 2 ? 1.5 : 4;
cr += (2 + (tg - 2) * (0.3 + 0.7 * m1) - cr) * 0.02;
cb = fx_comb(0, mid0, max(10, freq * cr), 0.3 + m1 * 0.45);
cmix = (0.25 + m4 * 0.6) * move;
mid1 = mid0 + cb * cmix;
ph = beat * 4 - floor(beat * 4);
gdep = m4 * move;
gt = (ph < 0.6 || gdep < 0.05) ? 1 : 1 - gdep * 0.7;
g += (gt - g) * 0.02;
mid = fx_sat(fx_ott(0, mid1, m7), 0.35);
sub = osc_sine(0, freq * 0.494257);
out = (sub * m8 * 0.5 + mid * 0.5) * env * (0.5 + vel * 0.5) * g;
```
```eel master
// crossover preserves mono low end while compressor glues upper band
loL = util_xover_lo(0, inl);
hiL = util_xover_hi(0);
loR = util_xover_lo(1, inr);
hiR = util_xover_hi(1);
hiL = fx_comp(0, hiL, 0.3);
hiR = fx_comp(1, hiR, 0.3);
outl = loL + hiL;
outr = loR + hiR;
```
