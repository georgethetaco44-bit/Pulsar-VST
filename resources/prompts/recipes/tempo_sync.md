# Tempo-synced motion
keywords: wobble, wub, lfo, gate, gated, trance gate, sidechain, pump, pumping, rhythmic, sync, tempo, pulse, pulsing, movement, moving, step, sequence
Use `mod_lfo(slot, beats, shape)` (`beats` is period in quarter notes: `1` = 1/4, `0.5` = 1/8, `0.25` = 1/16; `shape` 0–1 morphs sine/tri/saw/square/S&H, retriggers at note-on). Pick the division with a macro in steps; in a bass, move only the mid band.
```eel
per = m2 < 0.33 ? 1 : m2 < 0.66 ? 0.5 : 0.25;         // 1/4, 1/8, 1/16 note period
lfo = mod_lfo(0, per, m4 * 0.5);                      // 0..1, retriggers at note-on
x = flt_lp(0, osc_saw(0, freq), 0.2 + lfo * m1 * 0.6, 0.5);
ph = beat * 4 - floor(beat * 4);                      // position inside the 1/16
g += ((ph < 0.6 || m3 < 0.05 ? 1 : 1 - m3) - g) * 0.01;  // pumping gate
out = x * g * env_adsr(0, gate, 0.005, 0.3, 0.9, 0.3);
```
