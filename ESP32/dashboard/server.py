#!/usr/bin/env python3
"""Live current dashboard server (Python stdlib only; pyserial for live mode).

Option 1 - live data from the Arduino:
    python3 dashboard/server.py --serial /dev/ttyACM0 --baud 115200

Option 2 - demo, replays a ~3 min recording from a file:
    python3 dashboard/server.py --demo

Then open http://localhost:8000
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time
from collections import deque
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

from alerts import Alerter, load_config, public_view
from sources import ReplaySource, SerialSource

HERE = os.path.dirname(os.path.abspath(__file__))
STATIC = os.path.join(HERE, "static")
DEFAULT_DEMO = os.path.join(HERE, "data", "demo_current.csv")


class Hub:
    """Holds recent samples + thresholds and fans new samples out to browsers."""

    def __init__(self, low: float, high: float, history: int = 20000):
        self.lock = threading.Lock()
        self.cond = threading.Condition(self.lock)
        self.buf: deque[dict] = deque(maxlen=history)
        self.low, self.high = low, high
        self.mode = ""
        self.source = None
        self.alerter: Alerter | None = None
        self.total = 0
        self.lost = 0
        self._last_seq: int | None = None
        self._last_ts: int | None = None
        self._ts_base = 0  # added to device timestamps to survive Arduino resets

    def push(self, s) -> None:
        with self.cond:
            # Packet-loss accounting from the sequence number.
            if self._last_seq is not None:
                gap = s.seq - self._last_seq - 1
                if 0 < gap < 1000:
                    self.lost += gap
            self._last_seq = s.seq

            # Device timestamps restart when the Arduino resets: keep the axis monotonic.
            if self._last_ts is not None and s.timestamp_ms + self._ts_base < self._last_ts:
                self._ts_base = self._last_ts - s.timestamp_ms + 1
            t = s.timestamp_ms + self._ts_base
            self._last_ts = t

            self.total += 1
            if self.alerter:
                self.alerter.feed(t, s.current_ma, self.low, self.high)
            self.buf.append({"t": t, "i": s.current_ma, "seq": s.seq, "wall": time.time()})
            self.cond.notify_all()

    def meta(self) -> dict:
        return {
            "low": self.low,
            "high": self.high,
            "mode": self.mode,
            "status": getattr(self.source, "status", ""),
            "total": self.total,
            "lost": self.lost,
            "alerts": self.alerter.status() if self.alerter else None,
        }


def run_source(hub: Hub, source) -> None:
    for s in source:
        hub.push(s)


def make_handler(hub: Hub):
    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=STATIC, **kw)

        def log_message(self, *a):  # keep the terminal quiet
            pass

        def _json(self, obj, code=200):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/api/history":
                with hub.lock:
                    return self._json({"meta": hub.meta(), "samples": list(hub.buf)})
            if self.path == "/api/alerts/config":
                al = hub.alerter
                return self._json({"config": public_view(al.cfg), "status": al.status(),
                                   "history": al.history})
            if self.path == "/api/events":
                return self._stream()
            return super().do_GET()

        def _body(self) -> dict:
            n = int(self.headers.get("Content-Length", 0))
            return json.loads(self.rfile.read(n) or b"{}")

        def do_POST(self):
            al = hub.alerter
            if self.path == "/api/alerts/test":
                if not al.configured:
                    return self._json({"error": f"Push alerts not set up: {al.problem}"}, 400)
                al.send_test()
                return self._json(al.status())
            if self.path in ("/api/alerts/mute", "/api/alerts/config"):
                try:
                    body = self._body()
                    if self.path.endswith("/mute"):
                        body = {"enabled": not bool(body["muted"])}
                    if not isinstance(body, dict):
                        raise ValueError("expected a JSON object")
                    with hub.lock:
                        al.update_config(body)
                except (KeyError, ValueError) as e:   # JSONDecodeError is a ValueError
                    return self._json({"error": str(e)}, 400)
                except OSError as e:
                    return self._json({"error": f"could not save alerts.json: {e}"}, 500)
                return self._json({"config": public_view(al.cfg), "status": al.status()})
            if self.path != "/api/thresholds":
                return self._json({"error": "not found"}, 404)
            try:
                data = self._body()
                low, high = float(data["low"]), float(data["high"])
                if low >= high:
                    raise ValueError("low must be < high")
            except (ValueError, KeyError, json.JSONDecodeError) as e:
                return self._json({"error": str(e)}, 400)
            with hub.cond:
                hub.low, hub.high = low, high
                hub.cond.notify_all()
            return self._json(hub.meta())

        def _stream(self):
            """Server-Sent Events: batches of new samples + meta, ~20 updates/s max."""
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            with hub.lock:
                sent = hub.total
            try:
                while True:
                    with hub.cond:
                        hub.cond.wait(timeout=1.0)
                        new = hub.total - sent
                        batch = list(hub.buf)[-new:] if new > 0 else []
                        sent = hub.total
                        meta = hub.meta()
                    msg = json.dumps({"meta": meta, "samples": batch})
                    self.wfile.write(f"data: {msg}\n\n".encode())
                    self.wfile.flush()
                    time.sleep(0.05)
            except (BrokenPipeError, ConnectionResetError):
                pass

    return Handler


def main() -> None:
    sys.stdout.reconfigure(line_buffering=True)  # log lines appear immediately
    ap = argparse.ArgumentParser(description="Live current dashboard")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--serial", metavar="PORT", help="live mode, e.g. /dev/ttyACM0 or COM3")
    src.add_argument("--demo", nargs="?", const=DEFAULT_DEMO, metavar="CSV",
                     help="demo mode, replay a CSV (default: data/demo_current.csv)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--speed", type=float, default=1.0, help="demo replay speed multiplier")
    ap.add_argument("--no-loop", action="store_true", help="demo: stop at end of file")
    ap.add_argument("--low", type=float, default=200.0, help="lower threshold, mA")
    ap.add_argument("--high", type=float, default=800.0, help="upper threshold, mA")
    ap.add_argument("--port", type=int, default=8000, help="HTTP port")
    ap.add_argument("--alerts", default=os.path.join(HERE, "alerts.json"),
                    help="ntfy push-alert config (default: dashboard/alerts.json)")
    args = ap.parse_args()

    hub = Hub(args.low, args.high)
    if args.serial:
        hub.mode = "live"
        hub.source = SerialSource(args.serial, args.baud)
    else:
        if not os.path.exists(args.demo):
            raise SystemExit(f"{args.demo} missing - run: python3 dashboard/make_demo_data.py")
        hub.mode = "demo"
        hub.source = ReplaySource(args.demo, args.speed, loop=not args.no_loop)

    try:
        cfg = load_config(args.alerts)
    except (OSError, ValueError) as e:
        raise SystemExit(f"Could not read {args.alerts}: {e}")
    hub.alerter = Alerter(cfg, hub.mode, path=args.alerts)
    st = hub.alerter.status()
    if st["configured"]:
        print(f"Push alerts: ntfy topic '{st['topic']}' on {st['server']}" + (" (off)" if st["muted"] else ""))
    else:
        print(f"Push alerts: not set up ({st['problem']}) - use the ☰ menu in the dashboard")

    threading.Thread(target=run_source, args=(hub, hub.source), daemon=True).start()

    server = ThreadingHTTPServer(("0.0.0.0", args.port), make_handler(hub))
    server.daemon_threads = True
    print(f"[{hub.mode}] dashboard on http://localhost:{args.port}  (Ctrl+C to stop)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
