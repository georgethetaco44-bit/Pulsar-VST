# West coast complex oscillator (FM + wavefolder)
category: generative
keywords: west coast, complex oscillator, wavefolder, folding, fold, timbre, fm, through-zero, modular, bongo
West-coast synthesis generates complex harmonic timbres through linear through-zero FM and wavefolding rather than subtractive filtering. Modulate a sine carrier with a second sine at modest FM depth, then strike `fx_fold` with envelope-dependent drive so transients bloom with rich harmonics. Always include a DC trap after the wavefolder.
```eel
e = env_adsr(0, gate, 0.002, 0.18 + m2 * 0.3, 0.0, 0.05);
ratio = m3 < 0.33 ? 0.25 : m3 < 0.66 ? 0.5 : 0.375;
mod_idx = m1 * 2.0 * e;
raw = osc_fm(0, freq, ratio, mod_idx, 0.0);
folded = fx_fold(raw, e * (0.3 + m1 * 0.6));
dc += (folded - dc) * 20 / srate; folded -= dc;
x = flt_lp(0, folded, 0.1 + e * 0.75, 0.35);
out = x * (e * e);
```
