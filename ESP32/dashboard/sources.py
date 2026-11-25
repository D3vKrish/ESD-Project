"""Data sources for the current dashboard.

Every source yields `Sample` objects. The server does not care where they come from:
  * SerialSource  - live data from the Arduino over USB serial
  * ReplaySource  - demo data replayed from a CSV file at real speed

Wire format expected on serial (one line per sample, from the Arduino sketch in
dashboard/arduino/telemetry_serial/):

    <timestamp_ms>,<current_ma>,<seq>

Any line that does not look like that (debug prints, boot text) is ignored.
"""

from __future__ import annotations

import csv
import os
import re
import time
from dataclasses import dataclass
from typing import Iterator

_LINE = re.compile(r"^\s*(\d+)\s*,\s*(-?\d+(?:\.\d+)?)\s*,\s*(\d+)\s*$")

# SPI receiver debug format (node_t in microseconds, current in microamps):
#   [SPI RX] seq=17403  node_t=1740368274 us  I=22100 uA (22.1 mA)  dt_rx=... dt_node=...
# The serial stream drops characters, so records can be truncated or glued together.
# finditer() picks every complete record out of a line; the uA and mA values must agree
# so a record with a digit missing from the current is rejected.
_SPI = re.compile(
    r"seq=(\d+)\s+node_t=(\d+)\s*us\s+I=(-?\d+)\s*uA\s*\(\s*(-?\d+(?:\.\d+)?)\s*mA\s*\)"
)
_U32 = 1 << 32


@dataclass
class Sample:
    timestamp_ms: int   # device timestamp (millis() on the Arduino)
    current_ma: float   # INA219 current in mA
    seq: int            # packet sequence number


def parse_line(line: str) -> Sample | None:
    """Parse exactly `timestamp,current,seq` (spaces allowed). Anything else -> None,
    so debug prints such as "INA219 addr 0x40 ok" can never be mistaken for data."""
    m = _LINE.match(line)
    if not m:
        return None
    return Sample(int(m[1]), float(m[2]), int(m[3]))


def parse_spi_records(line: str) -> list[tuple[int, int, float]]:
    """All well-formed `[SPI RX]` records in a line -> [(seq, node_t_us, current_ma)]."""
    out = []
    for m in _SPI.finditer(line):
        ua, ma = int(m[3]), float(m[4])
        if abs(ua / 1000.0 - ma) > 0.051:   # one of the two numbers lost a digit
            continue
        out.append((int(m[1]), int(m[2]), ua / 1000.0))
    return out


class _TimingCheck:
    """Rejects records whose seq/node_t were damaged by dropped characters.

    A record is accepted only if, relative to the last accepted one, seq moved forward and
    node_t moved by about (seq difference) x (frame period). The period is learned from
    the first two records that agree with each other. After many rejections in a row
    (for example the node rebooted and seq restarted) it starts learning again.
    """

    def __init__(self):
        self.reset()

    def reset(self):
        self.last: tuple[int, int] | None = None   # (seq, node_t_us)
        self.pending: tuple[int, int] | None = None
        self.period: float | None = None           # us per seq step
        self.rejects = 0

    @staticmethod
    def _step(a, b):
        dseq = b[0] - a[0]
        dt = (b[1] - a[1]) % _U32   # node_t is a uint32 us counter, wraps at ~71 min
        return dseq, dt

    def accept(self, seq: int, t_us: int) -> bool:
        cur = (seq, t_us)
        if self.last is None:
            if self.pending is not None:
                dseq, dt = self._step(self.pending, cur)
                if 1 <= dseq <= 50 and 1_000 <= dt / dseq <= 10_000_000:
                    self.period = dt / dseq
                    self.last = cur
                    return True
            self.pending = cur
            return False

        assert self.period is not None
        dseq, dt = self._step(self.last, cur)
        ok =1 <= dseq <= 1000 and 0.8 <= (dt / dseq) / self.period <= 1.25
        if ok:
            self.period = 0.9 * self.period + 0.1 * (dt / dseq)
            self.last = cur
            self.rejects = 0
            return True
        self.rejects += 1
        if self.rejects >= 10:
            self.reset()
            self.pending = cur
        return False


class SpiLineDecoder:
    """Turns raw serial lines into Samples. Understands both formats:
    the plain `timestamp_ms,current_ma,seq` line and the `[SPI RX] ...` debug line."""

    def __init__(self):
        self.check = _TimingCheck()

    def feed(self, line: str) -> list[Sample]:
        s = parse_line(line)
        if s:
            return [s]
        out = []
        for seq, t_us, ma in parse_spi_records(line):
            if self.check.accept(seq, t_us):
                out.append(Sample(t_us // 1000, ma, seq))
        return out


class SerialSource:
    """Reads telemetry lines from the Arduino. Reconnects if the board is unplugged."""

    def __init__(self, port: str, baud: int = 115200):
        self.port = port
        self.baud = baud
        self.status = "connecting"

    def __iter__(self) -> Iterator[Sample]:
        import serial  # pyserial; only needed for live mode

        while True:
            try:
                with serial.Serial(self.port, self.baud, timeout=1) as ser:
                    self.status = f"live · {self.port} @ {self.baud}"
                    ser.reset_input_buffer()
                    dec = SpiLineDecoder()
                    while True:
                        raw = ser.readline()
                        if not raw:
                            continue
                        yield from dec.feed(raw.decode("ascii", errors="ignore"))
            except (serial.SerialException, OSError) as e:
                self.status = f"disconnected · retrying ({e.__class__.__name__})"
                time.sleep(1.0)


class ReplaySource:
    """Replays a CSV (timestamp_ms,current_ma,seq) with the original timing."""

    def __init__(self, path: str, speed: float = 1.0, loop: bool = True):
        self.path = path
        self.speed = speed
        self.loop = loop
        self.status = f"demo · {os.path.basename(path)}"

    def _rows(self) -> list[Sample]:
        rows = []
        with open(self.path, newline="") as f:
            for row in csv.reader(f):
                s = parse_line(",".join(row))
                if s:
                    rows.append(s)
        if not rows:
            raise SystemExit(f"No samples found in {self.path}")
        return rows

    def __iter__(self) -> Iterator[Sample]:
        rows = self._rows()
        offset = 0  # keeps timestamps increasing across loops
        while True:
            t0_dev = rows[0].timestamp_ms
            t0_wall = time.monotonic()
            for s in rows:
                due = t0_wall + (s.timestamp_ms - t0_dev) / 1000.0 / self.speed
                delay = due - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                yield Sample(s.timestamp_ms + offset, s.current_ma, s.seq)
            if not self.loop:
                self.status = "demo · finished"
                return
            offset += rows[-1].timestamp_ms - t0_dev + (rows[1].timestamp_ms - rows[0].timestamp_ms)
