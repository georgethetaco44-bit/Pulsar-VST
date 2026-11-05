# Wobble sequence (rate changes within the bar)
category: bass-square
keywords: riddim, wobble pattern, wub pattern, triplet, triplets, 1/8t, 1/16t, rhythm, rhythmic, sequence, half time, halftime, dubstep, 140, switch, switching, pattern
The heart of modern riddim and dubstep is a wobble whose rate switches within the bar (1/4, 1/8, 1/8T, 1/16, or rest). Grid-lock the phase to `beat` rather than `mod_lfo`; with one-beat steps and periods that divide one beat, the phase is zero at every step boundary so switching rate never clicks. A single phase drives cutoff, formant vowel, or fold together. The sub does not wobble, but stops on rests.
```eel
bar = beat - floor(beat / 4) * 4; step = floor(bar);
pat = m2 < 0.5 ? (step == 0 ? 1 : step == 1 ? 0.5 : step == 2 ? 0.3333 : 0.25)
               : (step == 0 ? 0.5 : step == 1 ? 0.5 : step == 2 ? 0.25 : 0);
ph = pat > 0 ? (beat / pat - floor(beat / pat)) : 0;
wub = pat > 0 ? (1 - ph) * (1 - ph) : 0;
w += (wub - w) * 0.02;
mid = fx_sat(flt_ladder(0, osc_square(0, freq * 2, 0.5), 0.15 + w * (0.3 + m1 * 0.4), 0.7), 0.5);
mid = flt_formant(0, mid, w * 0.8, 0.6);
mid = flt_hp(0, fx_ott(0, mid, 0.7), 0.27, 0);
sub = pat > 0 ? osc_sine(0, freq) : 0;
out = (sub * 0.5 + mid * 0.5) * env_adsr(0, gate, 0.002, 0.2, 0.8, 0.3);
```
