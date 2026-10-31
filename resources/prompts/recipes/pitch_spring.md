# Per-sample spring for pitch
keywords: boing, bounce, bouncy, rubber, cartoon, goofy, funny, wobble, springy, elastic, pitch, dive, drop, bend, zap, laser, wonky, wonk
Simulate a damped mass-spring per sample in semitones: kick it in init and the pitch overshoots and wobbles back to the note. Use the spring's speed to open a filter.
```eel
// init: sx = -(4 + m1 * 14); sv = 0;
w = 2 * $pi * (3 + m1 * 11); z = 0.05 + m2 * 0.5;
sv += (-w * w * sx - 2 * z * w * sv) / srate;
sx += sv / srate;
f = freq * exp(sx * 0.0577623);  // semitones -> ratio
sq = min(1, abs(sv) / (w * 10 + 1));
y = flt_lp(0, osc_square(0, f, 0.4), 0.3 + sq * m3 * 0.5, 0.7);
out = (osc_sine(0, f) * 0.8 + flt_hp(0, y, 0.247, 0) * 0.7) * env_adsr(0, gate, 0.002, 0.3, 0.8, 0.3);
```
