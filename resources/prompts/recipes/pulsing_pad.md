# Pulsing Pad
category: pad
keywords: pulsing, gate, rhythmic, tremolo, tempo gate, chop, pump, chord, trance
A tempo gate smoothly carves rhythmic pulses out of sustained chords. Using smooth cosine or exponential envelopes from `beat` prevents audio clicks while retaining pump.
```eel
env = env_adsr(0, gate, 0.2 + m1 * 0.3, 0.5, 0.85, 0.4);
a = osc_supersaw(0, freq, 0.2 + m2 * 0.5, 0.65);
b = osc_tri(0, freq * 0.5) * 0.3;
x = flt_lp(0, a * 0.7 + b, min(0.9, 0.2 + m3 * 0.5), 0.2);
// smooth tempo pump from beat
ph = beat * 2 - floor(beat * 2);
pump = 0.5 + 0.5 * cos(ph * 2 * $pi);
gateAmt = 1 - m4 * 0.8 * (1 - pump);
out = fx_sat(x, 0.2) * env * gateAmt * 0.5;
```
