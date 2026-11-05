# Jump-up bounce (boing + talking wah)
category: bass-square
keywords: jump up, jumpup, jump-up, dnb, drum and bass, bouncy, boing, boingy, warp, warpy, wobbly, squelch, squelchy, cheeky, playful, talking, 174
Playful jump-up mid basses combine a per-step pitch dive with a talking formant wah. Each 1/8 or 1/16 step re-triggers a fast 30–60 ms dive from +7..+12 semitones down to zero on the mid only, creating a boing on every step. Step or sweep `flt_formant` vowel for the talking wah, and add a resonant lowpass for squelch with a light ±1–2 semitone warp pitch wobble on a macro. The sub never warps.
```eel
rate = m2 < 0.5 ? 0.5 : 0.25;
sph = beat / rate - floor(beat / rate);
boing = max(0, 1 - sph * 5.0);
warp = sin(beat * 6.283185 * 2) * (m3 * 1.5);
mid_f = freq * pow(2, (boing * (7 + m1 * 5) + warp) / 12);
src = osc_square(0, mid_f, 0.45) * 0.7 + osc_saw(0, mid_f) * 0.3;
mid = flt_ladder(0, src, 0.2 + boing * 0.4, 0.65 + m4 * 0.25);
mid = flt_formant(0, mid, sph * 0.8 + m1 * 0.2, 0.6);
mid = fx_sat(flt_hp(0, fx_ott(0, mid, 0.7), 0.27, 0) * 1.5, 0.4);
out = (osc_sine(0, freq) * 0.5 + mid * 0.5) * env_adsr(0, gate, 0.002, 0.25, 0.8, 0.3);
```
