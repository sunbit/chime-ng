# Synthesized sound

`chime.wav` is newly synthesized for Chime NG and distributed under the project
MIT license. It contains no samples or recordings extracted from Ring firmware.
Format: PCM signed 16-bit little-endian, mono, 22,050 Hz, 114,176 frames.
Two decaying tones start at 0.15 s (880 Hz) and 0.85 s (659.255 Hz), each with a
12 ms attack, exponential decay and a quieter second harmonic. The remaining
tail is silence. `assets/generate.py` documents and recreates the synthesis.
Builds use the checked-in WAV. To regenerate it, run:

```sh
python3 assets/generate.py
```

Initial installation stores it at `/chime/original.wav`. The MQTT command
`original` plays the currently selected sound, including a replacement
uploaded through the web interface.
