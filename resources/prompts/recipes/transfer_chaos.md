# Piece-wise transfer functions and chaos modulation
keywords: chaos, chaotic, random, organic, drift, unpredictable, distort, distortion, grit, gritty, crunch, broken, asymmetric, shaper, waveshaper, lorenz, duffing
Write custom shapers inline (different curves for each half-wave) and modulate with chaos: rate near 0 is a slow LFO, near 1 audio rate. Asymmetric curves make DC: trap it.
```eel
c = chaos_lorenz(0, m5 * 0.2, 0);
x = osc_saw(0, freq * (1 + c * m6 * 0.05));
y = x > 0 ? sin(x * (1 + m1 * 8)) : floor(x * 4) / 4;
dc += (y - dc) * 20 / srate; y -= dc;
out = y * env_adsr(0, gate, 0.005, 0.3, 0.8, 0.3);
```
