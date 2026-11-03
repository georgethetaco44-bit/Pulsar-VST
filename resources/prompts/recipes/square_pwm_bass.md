# Square / PWM Bass
category: bass-square
keywords: square, pulse, pwm, bass, sub, riddim, garage, width, modulation
A pulse-width modulated square wave produces rich hollow overtones. Modulate pw between 0.1 and 0.9 with an LFO or macro. Detune a second square slightly for thickness.
```eel
// init: pw = 0.5;
lfo = mod_lfo(0, 0.5, 0);
pw = min(0.85, max(0.15, 0.5 + (lfo - 0.5) * 0.6 * m1));
a = osc_square(0, freq, pw);
b = osc_square(1, freq * 1.004, 1 - pw);
sq = (a + b) * 0.5;
env = env_adsr(0, gate, 0.01, 0.35, 0.8, 0.35);
x = flt_lp(0, sq, min(0.95, 0.2 + m2 * 0.5), m3 * 0.7);
x = fx_sat(x, 0.25 + m4 * 0.5);
sub = osc_sine(0, freq) * m5 * 0.7;
out = (sub + x * 0.6) * env;
```
