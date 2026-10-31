# Wavetable growl
keywords: wavetable, wt, table, morph, scan, modern, dubstep, brostep, hybrid, bass, tearout, metallic, gnarly, aggressive
Scan `osc_wt(slot, hz, table, pos)` (`table` 7 = growl, 6 = fold, 4 = sync, 2 = vocal) with an envelope and `mod_lfo`, then carve a moving notch (`flt_notch`) or formant (`flt_formant`) before `fx_shape` and `fx_ott`. Keep the sub clean and separate.
```eel
lfo = mod_lfo(0, m3 < 0.5 ? 0.5 : 0.25, m4 * 0.5);
pos = min(1, m1 * 0.6 + lfo * m2 * 0.5);
a = osc_wt(0, freq, 7, pos);
b = osc_wt(1, freq * 1.006, 6, pos * 0.8);
mid = flt_notch(0, a * 0.65 + b * 0.35, 0.35 + pos * 0.4, 0.6);
mid = flt_formant(0, fx_shape(mid, 0.3 + m1 * 0.5, m5), pos, 0.5);
mid = fx_sat(fx_ott(0, flt_hp(0, mid, 0.27, 0), 0.5 + m6 * 0.4), 0.3);
out = (osc_sine(0, freq) * 0.5 + mid * 0.5) * env_adsr(0, gate, 0.003, 0.3, 0.9, 0.35);
```
