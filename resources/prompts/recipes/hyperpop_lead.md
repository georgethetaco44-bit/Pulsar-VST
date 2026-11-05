# Hyperpop hypersaw lead (hard-tuned glides)
category: lead
keywords: hyperpop, digicore, glitchcore, bubblegum, hypersaw, hyper saw, sugary, bright, autotune, auto tune, hard tune, pitch snap, nightcore, cute
Hyperpop leads stack `osc_supersaw` and an octave-up square into bitcrush, hard clip, OTT, and a bright tilt filter. Smooth glide runs in voice code on log pitch (`log(x)/log(2)` since log2 does not exist). Blend the smooth glide with semitone-quantised pitch so slides jump in abrupt hard-tuned steps like extreme pitch correction. In master, add chorus, short delay, and light reverb.
```eel
target_p = log(max(freq, 20)) / log(2);
lp += (target_p - lp) * (0.005 + m2 * 0.03);
quant_p = floor(lp * 12 + 0.5) / 12;
p = lp + (quant_p - lp) * (0.5 + m1 * 0.5);
f = pow(2, p);
src = osc_supersaw(0, f, 0.4 + m3 * 0.4, 0.8) * 0.7 + osc_square(0, f * 2, 0.5) * 0.3;
x = fx_crush(0, src, m4 * 0.4, 1.0);
x = fx_shape(x, 0.5 + m1 * 0.5, 0);
x = flt_tilt(0, fx_ott(0, x, 0.5), 0.2 + m5 * 0.3);
out = x * env_adsr(0, gate, 0.002, 0.15, 0.85, 0.25);
```
