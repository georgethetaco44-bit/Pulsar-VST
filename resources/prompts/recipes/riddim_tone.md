# Riddim / tearout mid tone
category: bass-square
keywords: riddim, hollow, minimal, square, heavy, filthy, dirty, gnarly, wub, dubstep, tearout, low, grimy
Riddim character comes from one hollow, heavily compressed tone with lots of space rather than dense layers. Drive `osc_square` an octave up through hard clipping (`fx_shape` type 0), a moving notch, heavy `fx_ott`, and a highpass at 0.27 to leave room for the sub. Notes sit low (E1–G1). Leave gaps: the rhythm and the space are the sound.
```eel
lfo = mod_lfo(0, 0.5, 0);
wub = (1 - lfo) * (1 - lfo);
src = osc_square(0, freq * 2, 0.5) * 0.7 + osc_saw(0, freq * 2) * 0.3;
mid = fx_shape(src, 0.6 + m1 * 0.4, 0);
mid = flt_notch(0, mid, 0.2 + wub * 0.5, 0.75);
mid = fx_ott(0, mid, 0.6 + m2 * 0.3);
mid = flt_hp(0, mid, 0.27, 0);
mid = fx_sat(mid * 0.9, 0.3);
out = (osc_sine(0, freq) * 0.5 + mid * 0.5) * env_adsr(0, gate, 0.002, 0.2, 0.8, 0.3);
```
