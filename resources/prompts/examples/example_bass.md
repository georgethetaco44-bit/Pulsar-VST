category: bass
keywords: bass, sub, reese, neuro, dnb, drum and bass, dubstep, halftime, wobble, growl, riddim, wub, 808, low end, mid bass, foghorn, donk, jungle
prompt: dark detuned reese bass that snarls when you push it, with an optional tempo wobble
---
```json
{
  "name": "Snarl Reese",
  "voiceMode": "mono",
  "macros": [
    {"label": "Snarl", "default": 0.3},
    {"label": "Detune", "default": 0.45},
    {"label": "Cutoff", "default": 0.5},
    {"label": "Res", "default": 0.3},
    {"label": "Sub", "default": 0.7},
    {"label": "Wobble", "default": 0.0},
    {"label": "Rate", "default": 0.5},
    {"label": "Squash", "default": 0.4}
  ],
  "graph": {"edges": [["osc_supersaw:0", "flt_hp:0"], ["flt_hp:0", "flt_lp:0"], ["flt_lp:0", "fx_fold"], ["fx_fold", "util_safe_fb:0"], ["util_safe_fb:0", "osc_supersaw:0"], ["fx_fold", "out"], ["osc_sine:0", "out"], ["out", "fx_ott:0"]]}
}
```
```eel init
fb1 = 0; dc = 0;
```
```eel voice
// mids: detuned saws, pitch nudged by the last sample's output (safe feedback)
x = osc_supersaw(0, freq * (1 + fb1 * m1 * 0.01), m2, 0.6);
x = flt_hp(0, x, 0.247, 0);  // keep the mids off the sub (~110 Hz)
// tempo wobble on the cutoff: 1/4, 1/8 or 1/16 notes
div = m7 < 0.33 ? 1 : m7 < 0.66 ? 2 : 4;
lfo = sin(beat * 2 * $pi * div);
x = flt_lp(0, x, 0.3 + m3 * 0.5 + lfo * m6 * 0.2, m4 * 0.8);
// Snarl: drive and fold the mids only
x = fx_fold(fx_sat(x, 0.2 + m1 * 0.7), m1 * 0.6);
fb1 = util_safe_fb(0, x);
dc += (x - dc) * 20 / srate; x -= dc;
// a clean sine carries the low end
env = env_adsr(0, gate, 0.01, 0.3, 0.9, 0.3);
out = (osc_sine(0, freq) * m5 * 0.8 + x * 0.6) * env;
```
```eel master
// voices arrive mono: squash and glue once, copy to both sides
y = bus_sat(fx_ott(0, inl, m8), 0.3);
outl = y;
outr = y;
```
