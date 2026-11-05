# Self-playing ambient (random swells, no clock)
category: ambient
keywords: self-playing, self playing, self-generating, ambient modular, random swells, unclocked, free running, drifting, generative ambient, soundscape
Self-playing ambient patches run completely unclocked without relying on `beat` or host transport. A per-voice sample counter selects randomized rise times, fall times, and pitch degrees at the start of each swelling cycle. Changing pitch only when the envelope reaches zero ensures smooth transitions without clicks. Large master delays and lush reverb turn the slow drifts into expansive generative soundscapes.
```eel
timer <= 0 ? (
  f = freq * pow(2, floor(rand(1) * 7) * 2 / 12);
  period = (0.5 + rand(1) * 3.5) * (1.5 - m1 * 0.9) * srate;
  timer = period;
);
timer -= 1;
ph = timer / max(period, 1);
env = sin(ph * 3.14159265);
src = osc_sine(0, f) * 0.6 + osc_tri(0, f * 1.002) * 0.4;
x = flt_lp(0, src, 0.1 + env * (0.3 + m2 * 0.4), 0.2);
out = x * (env * env);
```
