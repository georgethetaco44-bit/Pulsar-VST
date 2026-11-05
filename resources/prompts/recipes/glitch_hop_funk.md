# Glitch-hop funk wobble (talkbox wah)
category: bass-square
keywords: glitch hop, glitch-hop, glitchhop, funky, funk, talkbox, talk box, wah, robot, robotic, 110, swing, laser, zap, stutter
Glitch-hop sits around 105–115 BPM and emphasizes funk groove over brutal aggression. A square and saw core runs through resonant filtering and a formant talkbox wah driven by envelope plus grid-locked 1/8T or 1/16 wobble. A macro introduces a laser zap attack dive, while a 1/32 gate stutter fires on the last beat of the bar.
```eel
bar = beat - floor(beat / 4) * 4;
stutter = (bar >= 3.0 && m3 > 0.1) ? min(1, (beat * 8 - floor(beat * 8)) * 4) : 1;
rate = m2 < 0.5 ? 0.3333 : 0.25;
ph = beat / rate - floor(beat / rate);
zap = env_adsr(1, gate, 0.001, 0.06, 0.0, 0.05) * m4 * 24.0;
f = freq * pow(2, zap / 12);
src = osc_square(0, f * 2, 0.5) * 0.6 + osc_saw(0, f * 2) * 0.4;
mid = flt_formant(0, src, 0.2 + (1 - ph) * 0.6, 0.65);
mid = fx_crush(0, mid, m5 * 0.5, 1.0);
mid = flt_hp(0, fx_ott(0, mid, 0.65), 0.27, 0);
out = (osc_sine(0, freq) * 0.5 + mid * 0.5) * stutter * env_adsr(0, gate, 0.002, 0.3, 0.8, 0.3);
```
