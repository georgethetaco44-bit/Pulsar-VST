# Generative Arp
category: generative
keywords: generative, arp, arpeggio, random, sequence, clock, scale, never repeats, melody, dice
A 16th-note clock driven by beat rolls random numbers to walk or jump across musical scale degrees. Octave shifts and probability steps keep the pattern evolving forever.
```eel
// init: last = -1; pos = 0; semi = 0;
step = floor(beat * 4);
step != last ? (
  last = step;
  h = sin(step * 117.13 + 31.41);
  r = abs(h * 43758.5453);
  r = r - floor(r);
  pos = (pos + (r > 0.5 ? 1 : 2)) % 5;
  pos == 0 ? semi = 0 : pos == 1 ? semi = 3 : pos == 2 ? semi = 7 : pos == 3 ? semi = 10 : semi = 12;
);
hoct = sin(step * 269.53 + 89.17);
roct = abs(hoct * 43758.5453);
roct = roct - floor(roct);
oct = roct < m1 * 0.5 ? 12 : 0;
f = freq * pow(2, (semi + oct) / 12);
ph = beat * 4 - floor(beat * 4);
senv = exp(-ph * (12 - m2 * 8));
x = osc_saw(0, f) * 0.6 + osc_square(0, f * 1.003, 0.5) * 0.4;
x = flt_lp(0, x, min(0.95, 0.15 + m3 * 0.4 + senv * 0.35), 0.3);
env = env_adsr(0, gate, 0.01, 0.2, 0.9, 0.2);
out = x * senv * env * 0.6;
```
