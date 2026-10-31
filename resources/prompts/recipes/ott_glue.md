# OTT glue
keywords: ott, squash, squashed, compressed, compression, glue, loud, punchy, modern, upfront, aggressive, polished, crispy
fx_ott is a 3-band upward + downward squash: detail up, bands even. Put it after the distortion, before the amp envelope; in a bass on the mid band only. On decaying sounds (plucks, keys) depth 0.3 at most and before any reverb, or it lifts noise and smear.
```eel
// voice, bass: x is the mid source
mid = fx_ott(0, flt_hp(0, fx_sat(x, 0.5), 0.27, 0), 0.3 + m8 * 0.6);
out = (osc_sine(0, freq) * 0.5 + fx_sat(mid, 0.3) * 0.5) * env;
// master, pluck or keys: light glue first, then space
gl = fx_ott(0, inl, m8 * 0.3); gr = fx_ott(1, inr, m8 * 0.3);
```
