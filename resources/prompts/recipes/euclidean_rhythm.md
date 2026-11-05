# Euclidean rhythms, clock divisions and ratchets
category: generative
keywords: euclidean, polyrhythm, polyrhythmic, clock, clock divider, division, ratchet, ratchets, pulses, steps, 3 against 4, off-kilter, idm
Euclidean rhythms distribute k pulses as evenly as possible across n steps using `floor(step * k / n) != floor((step - 1) * k / n)`. Adding a rotation offset shifts where the downbeat lands. Sub-step phases trigger ratchets, repeating the envelope 2–4 times inside a step. Combine different clock divisions for complex polyrhythmic patterns.
```eel
step = floor(beat * 4) + floor(m2 * 16);
pulses = floor(1 + m1 * 15); steps = 16;
hit = floor(step * pulses / steps) != floor((step - 1) * pulses / steps);
sph = beat * 4 - floor(beat * 4);
r_sub = m3 > 0.5 ? (sph * 2 - floor(sph * 2)) : sph;
e = hit ? max(0, 1 - r_sub * 4.0) : 0;
src = osc_tri(0, freq) * 0.7 + noise() * 0.3;
x = flt_lp(0, src, 0.1 + e * 0.7, 0.5);
out = x * (e * e);
```
