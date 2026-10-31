# mem[] ring buffer: stutter repeats and tape stop
keywords: stutter, repeat, glitch, chop, chopped, slice, tape, tape stop, slow down, slowdown, beat repeat, rhythmic, edit, cut up, gate, gated
Record the master into a mem[] ring buffer and read it back: repeat the last 1/16 or 1/32 on chosen steps (stutter), or read with a head that slows down (tape stop). Ramp edges over ~0.3 ms so cuts don't click.
```eel
// master: m1 stutters odd 1/16 steps with 1/32 repeats; m2 stops the tape
!ready ? (ready = 1; n = 65536; last = -1);
mem[w] = inl;
step = floor(beat * 4);
step != last ? (last = step; k = 0; len = floor(srate * 60 / bpm / 8);
  rep = (step % 2) && m1 > 0.3; anchor = (w - len + n) % n);
ed = srate * 0.0003; kk = k % len;
y = rep ? mem[(anchor + kk) % n] * min(1, min(kk, len - kk) / ed) : inl;
// tape stop on m2: read head speed falls from 1 to 0
spd += ((1 - m2) - spd) * 0.001; r += spd; r >= n ? r -= n;
y = m2 > 0.01 ? mem[floor(r)] * spd : y; m2 <= 0.01 ? r = w;
k += 1; w = (w + 1) % n;
outl = y; outr = y;
```
