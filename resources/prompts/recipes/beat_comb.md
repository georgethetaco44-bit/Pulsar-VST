# Beat-synced comb movement
keywords: comb, metallic, robotic, robot, vocal, talking, formant, sequence, sequenced, steps, rhythmic, neuro, metal, ringing, resonant, tuned
A comb tuned to a ratio of the note that steps every 1/16 note, with a short glide between steps, gives sequenced metallic movement. Works in voice (fx_comb at freq * ratio) or in master.
```eel
// init: cr = 2;
st = floor(beat * 4) % 4;
tg = st == 0 ? 2 : st == 1 ? 3 : st == 2 ? 1.5 : 4;
cr += (2 + (tg - 2) * m2 - cr) * 0.002;  // ~1 ms glide
x = osc_saw(0, freq);
y = x * 0.5 + fx_comb(0, x, freq * cr, 0.4 + m1 * 0.5) * 0.5;
out = (osc_sine(0, freq) * 0.8 + flt_hp(0, y, 0.247, 0) * 0.6) * env_adsr(0, gate, 0.002, 0.3, 0.8, 0.3);
```
