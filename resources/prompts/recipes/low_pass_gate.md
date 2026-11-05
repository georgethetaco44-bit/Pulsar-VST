# Low pass gate (organic bongo pluck)
category: pluck
keywords: low pass gate, lpg, bongo, vactrol, organic, plucky, woody, ping, percussive, west coast, modular
A low pass gate couples filter cutoff and output level to a single percussive envelope, simulating the slow decay of an optical vactrol. Combining a fast initial drop with a long exponential tail produces an organic acoustic response. Modulating both cutoff and level together—with level falling quadratically (`amp = e * e`)—creates realistic woody bongo plucks.
```eel
hit ? (e_fast = 1.0; e_slow = 1.0; hit = 0);
e_fast += (0 - e_fast) * (0.005 + (1 - m1) * 0.02);
e_slow += (0 - e_slow) * 0.0008;
e = e_fast * 0.7 + e_slow * 0.3;
cut = min(0.95, 0.08 + e * (0.5 + m2 * 0.35));
amp = e * e;
src = osc_sine(0, freq) + osc_tri(0, freq * 2) * 0.3;
x = flt_lp(0, src, cut, 0.45 + m3 * 0.3);
out = x * amp;
```
