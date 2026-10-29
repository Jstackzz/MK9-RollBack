#!/usr/bin/env python3
"""Writes launcher/data/sounds/match-found.wav, the chime the launcher plays when a Ranked or Kasual match is found
(Settings > Online > "Play a sound when a match is found").

Synthesized here, nothing recorded: two quick strikes a fourth apart (A5, then D6 held), each a bell of a few
inharmonic partials with a soft attack and an exponential decay, a little echo after them. 44.1 kHz, 16-bit mono,
about 0.9 s, peaking at -3 dBFS. The output is the same on every run.

    python launcher/tools/make_chime.py [out.wav]
"""

from __future__ import annotations

import math
import os
import struct
import sys
import wave

RATE = 44100
LENGTH = 0.9  # seconds
PEAK = 10 ** (-3 / 20)  # -3 dBFS

# (ratio to the strike's pitch, level, decay in seconds): a bell's partials, the higher ones dying first
PARTIALS = [(1.0, 1.0, 0.42), (2.0, 0.45, 0.22), (2.76, 0.28, 0.14), (5.4, 0.12, 0.06)]
# (start in seconds, pitch in Hz, level)
STRIKES = [(0.0, 880.0, 0.8), (0.11, 1174.66, 1.0)]
ECHO_DELAY, ECHO_LEVEL = 0.075, 0.22


def strike(t: float, pitch: float) -> float:
    if t < 0:
        return 0.0
    attack = min(1.0, t / 0.004)  # 4 ms: no click
    return attack * sum(level * math.exp(-t / decay) * math.sin(2 * math.pi * pitch * ratio * t)
                        for ratio, level, decay in PARTIALS)


def render() -> list[float]:
    n = int(RATE * LENGTH)
    dry = [sum(level * strike(i / RATE - start, pitch) for start, pitch, level in STRIKES) for i in range(n)]
    lag = int(RATE * ECHO_DELAY)
    wet = [dry[i] + (ECHO_LEVEL * dry[i - lag] if i >= lag else 0.0) for i in range(n)]
    fade = int(RATE * 0.05)  # the tail's last 50 ms to silence
    for i in range(fade):
        wet[n - 1 - i] *= i / fade
    top = max(abs(x) for x in wet) or 1.0
    return [x * PEAK / top for x in wet]


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "data", "sounds", "match-found.wav")
    out = os.path.normpath(out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    samples = render()
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", round(s * 32767)) for s in samples))
    print(f"{out}: {len(samples) / RATE:.2f} s, {os.path.getsize(out)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
