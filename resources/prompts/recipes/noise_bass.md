# Noise Bass
category: bass-reese
keywords: noise, texture, comb, formant, gritty, crunch, metallic, sub bass
Filtered noise through a tuned comb filter or formant filter creates an aggressive, textured mid-bass layer. Anchor the foundation with an untouched sine sub below 130 Hz.
```eel
n = noise() * 0.6;
mid = flt_bp(0, n, 0.3 + m1 * 0.4, 0.5);
mid = fx_comb(0, mid, max(20, freq * 2), 0.4 + m2 * 0.4);
mid = fx_sat(mid, 0.3 + m3 * 0.4);
mid = flt_hp(0, mid, 0.26, 0);
sub = osc_sine(0, freq) * m4 * 0.8;
env = env_adsr(0, gate, 0.01, 0.35, 0.8, 0.3);
out = (sub * 0.8 + mid * 0.5) * env;
```
