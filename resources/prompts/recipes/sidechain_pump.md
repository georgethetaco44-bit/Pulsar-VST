# Master sidechain pump
category: fx
keywords: sidechain, side chain, pump, pumping, ducking, duck, breathing, kick duck, future bass, melodic dubstep, house
In master, duck the mix synchronously from `beat` to mimic an aggressive kick drum sidechain. The pump curve falls sharply on the beat and recovers over the bar, with smoothing to prevent clicks. Place the ducking before reverb so the tail breathes with the groove. Never duck below about -18 dB to keep the low end grounded.
```eel
// master
rate = m7 < 0.5 ? 1.0 : 0.5;
ph = beat / rate - floor(beat / rate);
k = 3.0 + m8 * 5.0;
duck = 1.0 - (0.3 + m6 * 0.55) * max(0, 1.0 - ph * k);
duck_smooth += (duck - duck_smooth) * 0.05;
l = inl * duck_smooth;
r = inr * duck_smooth;
outl = bus_reverb(l, r, 0.35, 0.15);
outr = bus_reverb_r();
```
