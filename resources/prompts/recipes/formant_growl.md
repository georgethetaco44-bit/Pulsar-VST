# Formant growl (talking bass)
keywords: growl, talk, talking, vowel, yoi, wah, formant, vocal, mouth, nasal, tearout, dubstep, riddim, speak, yapping
Drive a rich source (`osc_wt` table 2 vocal or saw+pulse) through `flt_formant(slot, in, vowel, res)` (`vowel` 0–1 morphs a→e→i→o→u). Sweep `vowel` with the hero macro and `mod_lfo`.
```eel
lfo = mod_lfo(0, m2 < 0.5 ? 0.5 : 0.25, 0);                 // 1/8 or 1/16 tempo LFO
v = min(1, m1 * 0.65 + lfo * m2 * 0.5);                      // vowel a->e->i->o->u
x = fx_sat(osc_wt(0, freq, 2, v) + osc_saw(0, freq * 1.005) * 0.4, 0.4 + m1 * 0.4);
y = flt_formant(0, x, v, 0.5 + m3 * 0.4);
y = fx_sat(flt_hp(0, y, 0.27, 0) * 1.8, 0.35);
out = (osc_sine(0, freq) * 0.5 + y * 0.5) * env_adsr(0, gate, 0.002, 0.3, 0.9, 0.35);
```
