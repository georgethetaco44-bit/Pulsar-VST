# Band split in master
keywords: bass, sub, distort, distortion, drive, resample, heavy, grind, crush, crushed, neuro, dubstep, metallic, clean sub, multiband, wide, stereo
Do the heavy processing once in master and only on the band above 100 Hz: util_xover_lo/hi work in master too (own slots). Leave the low band dry and mono, drive the high band, then clip it into the headroom the sub leaves at that sample.
```eel
// master (inl == inr: the voices are mono)
lo = util_xover_lo(0, inl);
hi = util_xover_hi(0);
hi = fx_ott(0, fx_sat(hi * (1 + m1 * 3), 0.3 + m1 * 0.6), m8);
hi = fx_crush(0, hi, m2 * 0.5, 1 - m2 * 0.6);
hm = max(0.15, 1 - abs(lo));
y = hm * fx_sat(hi * 1.5 / hm, 0) + lo;
outl = y; outr = y;
```
