# Wave surgery (zero-crossing splicing)
keywords: switch, splice, morph, alternate, glitch, edit, chop, flip, between, engines, mutate, swap
Switch between engines on upward zero-crossings of a reference oscillator so the splice never clicks. Run every engine every sample so their phases stay continuous.
```eel
ref = osc_sine(0, freq);
(prev <= 0 && ref > 0) ? flip = floor(beat * 4) % 2;  // decide on 1/16s, apply at the crossing
prev = ref;
a = osc_saw(0, freq);
b = fx_fold(osc_tri(0, freq) * 2, m1);
out = (flip ? a : b) * env_adsr(0, gate, 0.002, 0.3, 0.8, 0.2);
```
