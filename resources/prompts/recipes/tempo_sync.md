# Tempo-synced motion
keywords: wobble, wub, lfo, gate, gated, trance gate, sidechain, pump, pumping, rhythmic, sync, tempo, pulse, pulsing, movement, moving, step, sequence
`beat` counts quarter notes, so sin(beat * 2 * $pi * n) is an LFO at n per beat (2 = 1/8 notes) and floor(beat * 4) is the 1/16 step. Choose the division with a macro in steps; smooth gates over a few ms.
```eel
div = m2 < 0.33 ? 1 : m2 < 0.66 ? 2 : 4;          // 1/4, 1/8, 1/16
lfo = 0.5 - 0.5 * cos(beat * 2 * $pi * div);       // 0..1, starts closed
x = flt_lp(0, osc_saw(0, freq), 0.2 + lfo * m1 * 0.6, 0.5);
ph = beat * 4 - floor(beat * 4);                    // position inside the 1/16
g += ((ph < 0.6 || m3 < 0.05 ? 1 : 1 - m3) - g) * 0.01;  // pumping gate
out = x * g * env_adsr(0, gate, 0.005, 0.3, 0.9, 0.3);
```
