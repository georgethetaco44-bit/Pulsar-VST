# Shift-register melody (loops, then mutates)
category: generative
keywords: shift register, looping random, locked random, mutating, evolving melody, modular, sequence, loop, motif, generative, idm
A 16-bit shift register seeded from note produces phrases that repeat identically until mutated. On each step, the low bit is flipped with a Change probability (m1: 0 = locked loop, 0.5 = drifting motif, 1 = random) and rotated back at the top. Read the low 8 bits as a control voltage, then quantise to scale degrees stored in `mem[]` during init. Unlike pure random dice, the repeating loop forms memorable motifs.
```eel
step = floor(beat * 4);
step != prev_step ? (
  bit = reg & 1; rand(1) < m1 ? bit = 1 - bit;
  reg = floor(reg / 2) + bit * (m3 < 0.5 ? 128 : 32768);
  volt = (reg & 127) / 127;
  deg = floor(volt * 5); oct = floor(volt * 2);
  semi = mem[deg] + oct * 12;
  f = freq * pow(2, semi / 12);
  prev_step = step;
);
env = env_adsr(0, gate, 0.002, 0.15, 0.0, 0.05);
out = osc_sine(0, f) * env;
```
