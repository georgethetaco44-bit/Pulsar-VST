# One-Finger Chords
category: chords
keywords: chord, chords, one finger, minor 7, minor 9, maj7, sus2, fifth, stab, house, stabs
Stacking multiple oscillators tuned to musical intervals lets a single key trigger complete chords. A shared ladder filter and snappy decay envelope yield classic house stabs.
```eel
// intervals: root, minor 3rd (1.1892), 5th (1.4983), minor 7th (1.7818)
v0 = osc_saw(0, freq) * 0.35;
v1 = osc_saw(1, freq * 1.189207) * 0.3;
v2 = osc_saw(2, freq * 1.498307) * 0.25;
v3 = osc_saw(3, freq * 1.781797) * 0.25;
chord = v0 + v1 + v2 + v3;
fenv = env_adsr(1, gate, 0.005, 0.25 + m1 * 0.2, 0.0, 0.2);
env = env_adsr(0, gate, 0.008, 0.4 + m2 * 0.3, 0.2, 0.3);
cut = min(0.95, max(0.08, 0.15 + m3 * 0.35 + fenv * (0.2 + m4 * 0.4)));
x = flt_ladder(0, chord, cut, 0.2 + m5 * 0.5);
out = fx_sat(x, 0.25) * env;
```
