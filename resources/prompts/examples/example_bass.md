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
    {"label": "Detune", "default": 0.4},
    {"label": "Cutoff", "default": 0.5},
    {"label": "Res", "default": 0.35},
    {"label": "Sub", "default": 0.8},
    {"label": "Wobble", "default": 0.0},
    {"label": "Rate", "default": 0.5},
    {"label": "Squash", "default": 0.4}
  ],
  "graph": {"edges": [["osc_supersaw:0", "flt_lp:0"], ["flt_lp:0", "fx_fold"], ["fx_fold", "fx_ott:0"], ["fx_ott:0", "out"], ["osc_sine:0", "out"]]}
}
```
```eel init
fb1 = 0;
```
```eel voice
// mids: supersaw + a saw detuned the other way; the last sample nudges the pitch
det = 1 + (m2 - 0.5) * 0.02;
a = osc_supersaw(0, freq * det * (1 + fb1 * m1 * 0.02), m2, 0.65);
b = osc_saw(0, freq * (2 - det));
env = env_adsr(0, gate, 0.01, 0.35, 0.8, 0.35);
// the envelope opens the filter; Wobble adds a 1/4, 1/8 or 1/16 LFO
div = m7 < 0.33 ? 1 : m7 < 0.66 ? 2 : 4;
lfo = sin(beat * 2 * $pi * div) * m6 * 0.25;
x = flt_lp(0, a * 0.7 + b * 0.3, min(0.95, 0.15 + m3 * 0.5 + env * 0.25 + lfo), m4 * 0.8);
// Snarl drives and folds the mids only; the loop is made safe
x = fx_fold(fx_sat(x, 0.25 + m1 * 0.65), m1 * 0.35);
fb1 = util_safe_fb(0, x);
// clear 20-130 Hz, squash after the drive, fixed ceiling
x = fx_sat(fx_ott(0, flt_hp(1, x, 0.27, 0), m8 * 0.8), 0.3);
// a clean sine sub joins at the end
out = (osc_sine(0, freq) * m5 * 0.6 + x * 0.5) * env;
```
```eel master
// the voice did the mix work: keep the sum clean
outl = inl;
outr = inr;
```
