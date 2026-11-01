# Glitch micro-edits
keywords: glitch, glitchy, edit, edits, chop, chopped, stutter, switch, splice, cut, cut up, broken, micro, 8 bit, bitcrush, crushed, mutate, metallic, inharmonic, frequency shift, shifter
Decide once per 1/16 from a hash of the step (the same pattern every bar), then switch engines, ramp the pitch, shift the frequency (fx_fshift: inharmonic metal) or mute. Every gain change is a 0.3 ms slew: sharp, never a click. Run every engine every sample so phases stay continuous.
```eel
// init: last = -1;
s = floor(beat * 4);
s != last ? (last = s; t0 = 0; h = sin(s * 12.9898) * 43758.5; h -= floor(h);
  eng = h < m1 * 0.5; ramp = h > 1 - m1 * 0.3; mute = h > 0.5 && h < 0.5 + m1 * 0.2);
sl = 1 / (0.0003 * srate); t0 += 1;
f = freq * exp((ramp ? 12 - 12 * min(1, t0 * bpm / (srate * 15)) : 0) * 0.0577623);
a = osc_saw(0, f);
b = fx_crush(0, osc_square(0, f, 0.3), 0.3 + m2 * 0.5, 0.6);
g = eng ? min(1, g + sl) : max(0, g - sl);
gm = mute ? max(0, gm - sl) : min(1, gm + sl);
x = fx_fshift(0, a * (1 - g) + b * g, h > 0.8 ? 0.5 + m3 * 0.3 : 0.5); // some steps shift up to +500 Hz
mid = fx_sat(flt_hp(0, x, 0.27, 0) * 1.5, 0.4) * gm;
out = (osc_sine(0, freq) * 0.5 + mid * 0.5) * env_adsr(0, gate, 0.001, 0.3, 0.9, 0.3);
```
