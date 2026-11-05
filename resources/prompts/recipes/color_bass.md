# Color bass (a growl that rings as a chord)
category: chords
keywords: color bass, colour bass, chord bass, melodic dubstep, melodic bass, chordal, harmonic, pitched growl, resonator, rings, future bass
Color bass is a growl tuned to resonate a distinct chord. The efficient design layers three oscillators tuned to root, third (minor 1.1892 or major 1.2599), and fifth (1.4983) through a shared growl and formant chain, then blends in a clean sine sub. A 1/8 wobble modulates cutoff, and master bus ducking creates the classic pumping sidechain feel.
```eel
third = m2 < 0.5 ? 1.1892 : 1.2599;
ch = osc_saw(0, freq * 2) * 0.4 + osc_saw(1, freq * 2 * third) * 0.35 + osc_saw(2, freq * 2 * 1.4983) * 0.25;
wub = mod_lfo(0, 0.5, 0);
mid = fx_sat(flt_ladder(0, ch, 0.15 + (1 - wub) * 0.5, 0.65), 0.3 + m1 * 0.5);
mid = flt_formant(0, mid, wub * 0.8, 0.55);
mid = flt_hp(0, fx_ott(0, mid, 0.7), 0.27, 0);
out = (osc_sine(0, freq) * 0.5 + mid * 0.5) * env_adsr(0, gate, 0.002, 0.3, 0.8, 0.3);
```
