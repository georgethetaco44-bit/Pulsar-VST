# OTT glue
keywords: ott, squash, squashed, compressed, compression, glue, loud, punchy, modern, upfront, aggressive, polished, crispy
fx_ott is a 3-band upward + downward squash: it brings up detail and evens out the bands. Put it in master on the whole sound, one slot per channel, usually on a macro; in a bass, put it on the high band only so the sub stays untouched.
```eel
// master
outl = bus_sat(fx_ott(0, inl, m8), 0.25);
outr = bus_sat(fx_ott(1, inr, m8), 0.25);
```
