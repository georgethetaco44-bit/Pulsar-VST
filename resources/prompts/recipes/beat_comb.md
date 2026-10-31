# Beat-synced comb and notch movement
keywords: comb, notch, phaser, flange, flanger, metallic, metal, robotic, robot, ringing, resonant, tuned, sequence, sequenced, steps, rhythmic, neuro, stab
A comb tuned to a ratio of the note steps every 1/16 with a ~1 ms glide: sequenced metal. Subtract it instead of adding it and the peaks become moving notches (hollow, phaser-like). Comb the driven mid band only.
```eel
// init: cr = 2;
st = floor(beat * 4) % 4;
tg = st == 0 ? 2 : st == 1 ? 3 : st == 2 ? 1.5 : 4;
cr += (2 + (tg - 2) * m2 - cr) * 0.01;  // ~1 ms glide
x = flt_hp(0, fx_sat(osc_saw(0, freq), 0.3 + m1 * 0.5), 0.27, 0);
y = x + fx_comb(0, x, freq * cr, 0.3 + m1 * 0.6) * (0.7 - m3 * 1.4);  // m3: peaks -> notches
out = (osc_sine(0, freq) * 0.5 + fx_sat(y, 0.4) * 0.5) * env_adsr(0, gate, 0.002, 0.3, 0.85, 0.3);
```
