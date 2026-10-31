# Cross-adaptive exciter injection
keywords: physical, resonator, string, pluck, bell, mallet, struck, ringing, gong, acoustic, organic, wood, glass, modal
Drive a physical model with a synth signal instead of a click, so its tone follows the synth. Keep the exciter quiet; phys_* are heavy, so one per patch.
```eel
ex = osc_supersaw(0, freq * 1.01, m1, 0.5) * 0.3;
out = phys_resonator(0, ex, freq, m2, m3, m4) * env_adsr(0, gate, 0.002, 0.4, 0.6, 0.3);
```
