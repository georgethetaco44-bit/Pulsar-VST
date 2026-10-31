# mem[] ring buffer: stutter repeats and tape stop
keywords: stutter, repeat, glitch, chop, chopped, slice, tape, tape stop, slow down, slowdown, halt, beat repeat, rhythmic, cut up, gate, gated
Record the master into a mem[] ring buffer and read it back: repeat the last 1/32 on chosen 1/16 steps (stutter), or read with a head that slows to a halt (tape stop; read between samples so it stays smooth). Ramp edges over 0.3 ms so cuts never click.
```eel
// master: m1 stutters odd 1/16 steps; m2 stops the tape
!ready ? (ready = 1; n = 65536; last = -1);
mem[w] = inl;
step = floor(beat * 4);
step != last ? (last = step; k = 0; len = floor(srate * 60 / bpm / 8);
  rep = (step % 2) && m1 > 0.3; anchor = (w - len + n) % n);
ed = srate * 0.0003; kk = k % len;
y = rep ? mem[(anchor + kk) % n] * min(1, min(kk, len - kk) / ed) : inl;
spd += ((1 - m2) - spd) * 0.0005; r += spd; r >= n ? r -= n;
i = floor(r); fr = r - i;
m2 > 0.01 ? (y = (mem[i] * (1 - fr) + mem[(i + 1) % n] * fr) * min(1, spd * 4)) : (r = w);
k += 1; w = (w + 1) % n;
outl = y; outr = y;
```
