#!/usr/bin/env python3
"""Rebuild the independently synthesized MIT-licensed two-note sound asset."""
from pathlib import Path
import math
import struct
import wave

RATE = 22050
FRAMES = 114176
samples = []
for index in range(FRAMES):
    t = index / RATE
    value = 0.0
    for start, frequency in [(0.15, 880.0), (0.85, 659.255)]:
        u = t - start
        if 0 <= u < 2.0:
            attack = min(1.0, u / 0.012)
            envelope = attack * math.exp(-3.0 * u)
            value += 0.36 * envelope * (math.sin(2 * math.pi * frequency * u) +
                                       0.18 * math.sin(2 * math.pi * 2 * frequency * u))
    samples.append(max(-32768, min(32767, round(value * 32767))))
output = Path(__file__).resolve().parents[1] / 'assets/chime.wav'
with wave.open(str(output), 'wb') as stream:
    stream.setparams((1, 2, RATE, 0, 'NONE', 'not compressed'))
    stream.writeframes(struct.pack('<' + str(FRAMES) + 'h', *samples))
print(output)
