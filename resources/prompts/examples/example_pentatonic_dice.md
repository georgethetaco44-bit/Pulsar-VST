category: generative
keywords: generative, arp, arpeggio, sequence, random, never repeats, hold one note, melody, music box
prompt: hold one note: generative pentatonic arp, random 1/16 steps, ping-pong delay, big reverb
---
```json
{
  "name": "Pentatonic Dice",
  "voiceMode": "mono",
  "macros": [
    {
      "label": "Chaos",
      "default": 0.6
    },
    {
      "label": "Bright",
      "default": 0.5
    },
    {
      "label": "Decay",
      "default": 0.4
    },
    {
      "label": "Range",
      "default": 0.5
    },
    {
      "label": "Res",
      "default": 0.3
    },
    {
      "label": "Delay",
      "default": 0.45
    },
    {
      "label": "Space",
      "default": 0.4
    },
    {
      "label": "Width",
      "default": 0.6
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
        "out"
      ],
      [
        "out",
        "bus_delay"
      ],
      [
        "bus_delay",
        "bus_reverb"
      ]
    ]
  }
}
```
```eel init
lastStep = -1000;
curPos = 2;
curSemi = 5;
```
```eel voice
// 16th-note clock driven by beat: rolls dice for scale step on each new 16th
curStep = floor(beat * 4);
curStep != lastStep ? (
  lastStep = curStep;
  h1 = sin(curStep * 117.13 + 31.41);
  r1 = abs(h1 * 43758.5453);
  r1 = r1 - floor(r1);
  h2 = sin(curStep * 269.53 + 89.17);
  r2 = abs(h2 * 43758.5453);
  r2 = r2 - floor(r2);
  randPos = floor(r2 * 7.99);
  seqPos = curPos + (r1 > 0.5 ? 1 : -1);
  seqPos > 7 ? seqPos = 0;
  seqPos < 0 ? seqPos = 7;
  // chaos macro blends between stepwise melodic walk and random jumps
  r1 < 0.15 + m1 * 0.85 ? curPos = randPos : curPos = seqPos;
  // minor pentatonic scale: 0, 3, 5, 7, 10, 12, 15, 17 semitones
  curPos == 0 ? curSemi = 0 : curPos == 1 ? curSemi = 3 : curPos == 2 ? curSemi = 5 : curPos == 3 ? curSemi = 7 : curPos == 4 ? curSemi = 10 : curPos == 5 ? curSemi = 12 : curPos == 6 ? curSemi = 15 : curSemi = 17
);
// Range jumps the top two steps a whole octave (never a fraction, so it stays in key)
extra = curPos >= 6 ? (m4 >= 0.5 ? 12 : 0) : 0;
f = freq * pow(2, (curSemi + extra) / 12);
// per-step snappy exponential decay envelope
ph = beat * 4 - floor(beat * 4);
stepEnv = exp(-ph * (13 - m3 * 10));
gateEnv = env_adsr(0, gate, 0.08, 0.3, 1, 0.35);
x = osc_saw(0, f) * 0.6 + osc_square(0, f * 1.004, 0.5) * 0.4;
cut = 0.12 + m2 * 0.35 + stepEnv * 0.45;
x = flt_lp(0, x, min(0.95, cut), m5 * 0.75);
x = fx_sat(x, 0.3);
out = x * stepEnv * gateEnv * (0.4 + vel * 0.6) * 1.8;
```
```eel master
// ping-pong tempo delay feeding spacious reverb with stereo widening
d = bus_delay(inl, inr, 0.75, 0.35, 0.1 + m6 * 0.35, 0.35);
dr = bus_delay_r();
w = bus_reverb(d, dr, 0.75, 0.1 + m7 * 0.35);
wr = bus_reverb_r();
outl = bus_width(w, wr, 0.3 + m8 * 0.5);
outr = bus_width_r();
```
