category: bass-square
keywords: square, pulse, pwm, riddim, garage, chopper, 8 bit, bass, sub, analog, glitch, snarl, crush, dnb, drum and bass, jungle, howl
prompt: huge analog square bass for drum and bass, mono, with glitch, snarl and crush macros
---
```json
{
  "name": "Howling Bass",
  "voiceMode": "mono",
  "macros": [
    {
      "label": "Glitch",
      "default": 0.0
    },
    {
      "label": "Snarl",
      "default": 0.0
    },
    {
      "label": "Cutoff",
      "default": 0.0
    },
    {
      "label": "Reso",
      "default": 1.0
    },
    {
      "label": "Sub",
      "default": 1.0
    },
    {
      "label": "Wobble",
      "default": 0.0
    },
    {
      "label": "Crush",
      "default": 0.0
    },
    {
      "label": "Squash",
      "default": 1.0
    }
  ],
  "graph": {
    "edges": [
      [
        "osc_square:0",
        "flt_hp:0"
      ],
      [
        "osc_square:1",
        "flt_hp:0"
      ],
      [
        "flt_hp:0",
        "flt_lp:0"
      ],
      [
        "flt_lp:0",
        "fx_fold"
      ],
      [
        "fx_fold",
        "out"
      ],
      [
        "osc_sine:0",
        "out"
      ],
      [
        "out",
        "util_xover_lo:0"
      ],
      [
        "util_xover_hi:0",
        "fx_ott:0"
      ]
    ]
  }
}
```
```eel init
dc = 0;
```
```eel voice
// mids: twin pulse-width modulated squares detuned against each other
pw = 0.5 + 0.22 * sin(beat * 2 * $pi * 2) * m6;
pw = min(0.9, max(0.1, pw));
a = osc_square(0, freq * 0.502896, pw);
b = osc_square(1, freq * 1.008 * 0.497120, 1 - pw);
mid = (a + b) * 0.5;
// clear sub from mids before drive; cutoff modulated by lfo and m3
mid = flt_hp(0, mid, 0.247, 0);
lfo = sin(beat * 2 * $pi * 2);
cut = 0.25 + m3 * 0.5 + lfo * m6 * 0.22;
cut = min(1, max(0.05, cut));
mid = flt_lp(0, mid, 0.364945, min(max(m4 * 0.85 + 0.22069, 0), 1));
// snarl drives and wavefolds the mid band
mid = fx_sat(mid * (1 + m2 * 2), 0.2 + m2 * 0.6);
mid = fx_fold(mid, m2 * 0.7);
dc += (mid - dc) * 20 / srate; mid -= dc;
// clean dedicated sine sub joins after mid drive
sub = osc_sine(0, freq * 0.502896) * m5;
env = env_adsr(0, gate, 0.05, 0.35, 0.9, 0.3);
out = (sub * 0.9 + mid * 0.7) * env;
```
```eel master
// crossover splits sub from highs so compression and glitch only touch top end
lo = util_xover_lo(0, inl);
hi = util_xover_hi(0);
hi = fx_sat(hi * (1 + m2 * 3), 0.3 + m2 * 0.5);
hi = fx_ott(0, hi, m8);
hi = fx_crush(0, hi, m7 * 0.6 + m1 * 0.25, 1 - m7 * 0.5);
hm = max(0.15, 1 - abs(lo));
y = hm * fx_sat(hi * 1.5 / hm, 0) + lo;
// glitch buffer repeats 1/8 or 1/16 beat chunks
!ready ? (ready = 1; n = 65536; w = 0; r = 0; k = 0; last = -1; len = 2000; anchor = 0; rep = 0);
mem[w] = y;
step = floor(beat * 4);
step != last ? (last = step; k = 0; tp = max(bpm, 20);
  len = m1 > 0.65 ? floor(srate * 60 / tp / 16) : floor(srate * 60 / tp / 8);
  len = min(8000, max(64, len));
  rep = m1 > 0.65 ? 1 : (m1 > 0.25 ? ((step % 2) == 1) : 0);
  anchor = (w - len + n) % n);
kk = k % max(len, 1);
ed = max(1, srate * 0.0003);
g = min(1, min(kk, len - kk) / ed);
y = rep ? mem[(anchor + kk) % n] * g + y * (1 - g) * 0.2 : y;
k += 1; w = (w + 1) % n;
outl = y; outr = y;
```
