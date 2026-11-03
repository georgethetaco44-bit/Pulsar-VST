# 808 Glide
category: bass-fm
keywords: 808, sub, glide, drop, pitch drop, boom, low end, kick, clean, pure
A booming 808 sub bass uses a clean sine oscillator with a snappy pitch drop at the note attack. Add gentle saturation for overtone warmth on small speakers.
```eel
pdrop = env_adsr(1, gate, 0.001, 0.08, 0.0, 0.1);
f = freq * (1 + pdrop * (0.5 + m1 * 1.5));
env = env_adsr(0, gate, 0.005, 0.8 + m2 * 1.2, 0.1, 0.4);
sub = osc_sine(0, f);
x = fx_sat(sub, 0.1 + m3 * 0.4);
out = x * env;
```
