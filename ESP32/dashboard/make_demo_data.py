#!/usr/bin/env python3
"""Generate a ~3 minute demo recording in the same shape the Arduino sends.

Columns: timestamp_ms,current_ma,seq   (20 Hz, int16 current like INA219_ReadCurrent_mA)

The profile deliberately exercises every dashboard state:
  normal load, a motor-start inrush spike above the high threshold, a slow ramp that
  crosses it, a brown-out dip below the low threshold, and a few dropped packets.
"""

import csv
import math
import os
import random

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "demo_current.csv")
RATE_HZ = 20
DURATION_S = 180
START_MS = 5_000  # Arduino has been up a few seconds before logging starts


def profile(t: float) -> float:
    if t < 20:
        base = 420
    elif t < 24:  # inrush: jumps to ~980 mA and decays
        base = 600 + 380 * math.exp(-(t - 20) * 1.6)
    elif t < 60:
        base = 600 + 25 * math.sin(t * 2.1)
    elif t < 90:  # slow ramp that crosses 800 mA near the end
        base = 600 + (t - 60) * 7.8
    elif t < 110:
        base = 520
    elif t < 125:  # brown-out dip below 200 mA
        k = (t - 110) / 15
        base = 520 - 400 * math.sin(math.pi * k)
    elif t < 150:
        base = 450
    else:  # oscillating load
        base = 500 + 210 * math.sin((t - 150) * 0.45)
    return base


def main() -> None:
    rng = random.Random(42)
    dropped = {812, 813, 1904, 2950, 2951, 2952}  # simulate lost CAN frames
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["timestamp_ms", "current_ma", "seq"])
        for n in range(DURATION_S * RATE_HZ):
            if n in dropped:
                continue
            t = n / RATE_HZ
            ts = START_MS + n * (1000 // RATE_HZ) + rng.randint(0, 2)  # small jitter
            i = profile(t) + rng.gauss(0, 9)
            w.writerow([ts, int(round(max(-32768, min(32767, i)))), n % 65536])
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
