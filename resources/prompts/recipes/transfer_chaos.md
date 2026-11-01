# Piece-wise transfer functions and chaos modulation
keywords: chaos, chaotic, random, organic, drift, unpredictable, never repeats, distort, distortion, grit, gritty, gnarly, crunch, broken, asymmetric, shaper, waveshaper, lorenz, duffing, phaser, phasing, swirl, swirly
Write custom shapers inline (a different curve for each half-wave) and modulate with chaos: rate near 0 is a slow LFO, near 1 audio rate. A phaser whose `pos` follows the chaos swirls without repeating. Asymmetric curves make DC; the mid band's high-pass traps it.
```eel
c = chaos_lorenz(0, 0.05 + m2 * 0.2, 0);
x = osc_saw(0, freq * (1 + c * m3 * 0.03));
y = x > 0 ? sin(x * (1 + m1 * 8)) : floor(x * 4) / 4;
y = fx_phaser(0, y, 0.5 + c * 0.4, m4, 0.5);   // notches drift with the attractor
y = fx_sat(flt_hp(0, y, 0.27, 0) * 1.5, 0.4);
out = (osc_sine(0, freq) * 0.5 + y * 0.5) * env_adsr(0, gate, 0.005, 0.3, 0.85, 0.3);
```
