category: texture
keywords: pad, texture, drone, ambient, atmosphere, evolving, chaos, chaotic, fm, scream, howl, metallic, alien, weird, glitch, noise, cinematic, eerie, dark
prompt: two oscillators that frequency-modulate each other one sample apart; the coupling goes from a clean tone to a chaotic howl
---
```json
{
  "name": "Coupled Howl",
  "voiceMode": "poly",
  "macros": [
    {"label": "Coupling", "default": 0.3},
    {"label": "Tone", "default": 0.45},
    {"label": "Loop Res", "default": 0.3},
    {"label": "Ratio", "default": 0.5},
    {"label": "Drift", "default": 0.3},
    {"label": "Attack", "default": 0.3},
    {"label": "Swell", "default": 0.3},
    {"label": "Space", "default": 0.4}
  ],
  "graph": {"edges": [["osc_a", "util_safe_fb:0"], ["util_safe_fb:0", "osc_b"], ["osc_b", "flt_lp:0"], ["flt_lp:0", "util_safe_fb:1"], ["util_safe_fb:1", "osc_a"], ["chaos_lorenz:0", "osc_a"], ["osc_a", "out"], ["osc_b", "out"]]}
}
```
```eel init
fb1 = 0; fb2 = 0; dc = 0;
```
```eel voice
// partner ratio: 1:1 beating, 3:2 or 2:1
r = m4 < 0.33 ? 1.003 : m4 < 0.66 ? 1.5 : 2.002;
// coupling (the hero) and the partner's tuning wander slowly with chaos
lz = chaos_lorenz(0, 0.1, 0);
cpl = m1 * m1 * 3 * (1 + lz * m5 * 0.5);
// two phase oscillators, each modulated by the other's previous sample
ph1 += freq / srate; ph1 -= floor(ph1);
ph2 += freq * r * (1 + lz * m5 * 0.004) / srate; ph2 -= floor(ph2);
y1 = sin(2 * $pi * ph1 + cpl * fb2);
y2 = sin(2 * $pi * ph2 + cpl * fb1);
fb1 = util_safe_fb(0, y1);
fb2 = util_safe_fb(1, flt_lp(0, y2, 0.3 + m2 * 0.6, m3 * 0.7));
y = y1 * 0.6 + y2 * 0.4;
dc += (y - dc) * 20 / srate; y -= dc;
env = env_adsr(0, gate, 0.35 + m6 * 0.3, 0.6, 0.8, 0.6);
out = y * env * 0.5;
```
```eel master
// a one-bar swell, wide chorus and a big room
sw = 0.5 + 0.5 * sin(beat * 2 * $pi * 0.25);
x = inl * (1 - m7 * 0.7 * sw);
cl = bus_chorus(x, x, 0.5, 0.5);
cr = bus_chorus_r();
outl = bus_reverb(cl, cr, 0.7, m8 * 0.5);
outr = bus_reverb_r();
```
