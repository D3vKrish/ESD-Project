"""Push alerts via ntfy (https://ntfy.sh) when current leaves the threshold range.

How it works: the server publishes to an ntfy *topic*; every phone/PC subscribed to that
topic in the ntfy app (Android / iOS / web) gets a push notification. No account needed.

Configure from the dashboard (☰ menu -> Push alerts); saved to dashboard/alerts.json.

Alert rules:
  * fire when current is out of range continuously for `hold_ms` (ignores noise spikes)
  * at most one alert per `cooldown_s` (shared by above-high and below-low)
  * optional "back to normal" notification when current returns in range
Notifications are sent on a background thread so a slow network never stalls data intake.
"""

from __future__ import annotations

import copy
import json
import os
import queue
import re
import threading
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(HERE, "alerts.json")

DEFAULTS = {
    "enabled": True,
    "server": "https://ntfy.sh",   # or your self-hosted ntfy server
    "topic": "",                   # anyone who knows the topic can read it: make it hard to guess
    "token": "",                   # optional access token (protected topics / self-hosted)
    "hold_ms": 50,
    "cooldown_s": 10,
    "notify_recovery": True,
}
SECRETS = {"token"}
ALERT_PRIORITY = 4      # ntfy "high" for out-of-range alerts
RECOVERY_PRIORITY = 3   # ntfy "default" for back-in-range
TOPIC_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
SERVER_RE = re.compile(r"^https?://[^\s/]+(:\d+)?(/[^\s]*)?$")


def load_config(path: str = CONFIG_PATH) -> dict:
    cfg = copy.deepcopy(DEFAULTS)
    if os.path.exists(path):
        with open(path) as f:
            saved = json.load(f)
        # only known keys: old WhatsApp-era fields in an existing file are dropped
        cfg.update({k: v for k, v in saved.items() if k in DEFAULTS})
    return cfg


def save_config(cfg: dict, path: str = CONFIG_PATH) -> None:
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(cfg, f, indent=2)
    os.replace(tmp, path)
    try:
        os.chmod(path, 0o600)   # may contain an access token
    except OSError:
        pass


def problems(cfg: dict) -> list[str]:
    """What is missing/invalid; empty list = ready to send."""
    out = []
    if not SERVER_RE.match(cfg["server"] or ""):
        out.append("server URL invalid")
    if not cfg["topic"]:
        out.append("topic missing")
    elif not TOPIC_RE.match(cfg["topic"]):
        out.append("topic may only use letters, digits, - and _")
    return out


def public_view(cfg: dict) -> dict:
    """Config for the browser: secrets replaced by has_<name> flags."""
    v = copy.deepcopy(cfg)
    for k in SECRETS:
        v[f"has_{k}"] = bool(v.get(k))
        v[k] = ""
    v["subscribe_url"] = f"{cfg['server'].rstrip('/')}/{cfg['topic']}" if cfg["topic"] else ""
    return v


def apply_update(cfg: dict, upd: dict) -> dict:
    """Validate a form submission and return the new config.
    Blank token = keep the saved one; send "clear_token": true to remove it."""
    new = copy.deepcopy(cfg)
    new.update({k: upd[k] for k in upd if k in DEFAULTS})
    if not str(upd.get("token", "")).strip():
        new["token"] = cfg.get("token", "")
    if upd.get("clear_token"):
        new["token"] = ""
    new["server"] = str(new["server"] or DEFAULTS["server"]).strip().rstrip("/")
    new["topic"] = str(new["topic"] or "").strip()
    new["token"] = str(new["token"] or "").strip()
    new["enabled"] = bool(new["enabled"])
    new["notify_recovery"] = bool(new["notify_recovery"])
    try:
        new["hold_ms"] = int(new["hold_ms"])
        new["cooldown_s"] = int(new["cooldown_s"])
    except (TypeError, ValueError):
        raise ValueError("hold and cooldown must be whole numbers") from None
    if not 0 <= new["hold_ms"] <= 60_000:
        raise ValueError("hold must be 0–60000 ms")
    if not 0 <= new["cooldown_s"] <= 86_400:
        raise ValueError("cooldown must be 0–86400 s")
    if not SERVER_RE.match(new["server"]):
        raise ValueError("server must be a URL like https://ntfy.sh")
    if new["topic"] and not TOPIC_RE.match(new["topic"]):
        raise ValueError("topic may only use letters, digits, - and _ (max 64)")
    return new


# ---------------------------------------------------------------- sender

def send_ntfy(cfg: dict, msg: dict) -> None:
    """Publish via ntfy's JSON API (UTF-8 safe titles, unlike header publishing)."""
    body = {"topic": cfg["topic"], "message": msg["text"], "title": msg["title"],
            "tags": msg.get("tags", []), "priority": msg.get("priority", 3)}
    req = urllib.request.Request(cfg["server"].rstrip("/") + "/", data=json.dumps(body).encode(),
                                 method="POST", headers={"Content-Type": "application/json"})
    if cfg.get("token"):
        req.add_header("Authorization", f"Bearer {cfg['token']}")
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            r.read()
    except urllib.error.HTTPError as e:
        try:
            detail = json.loads(e.read()).get("error", "")
        except ValueError:
            detail = ""
        hint = {401: " (token needed?)", 403: " (token not allowed on this topic)",
                429: " (rate limited, raise the cooldown)"}.get(e.code, "")
        raise RuntimeError(f"ntfy HTTP {e.code}: {detail}{hint}".strip()) from None
    except urllib.error.URLError as e:
        raise RuntimeError(f"cannot reach {cfg['server']}: {e.reason}") from None


# ---------------------------------------------------------------- alert engine

class Alerter:
    def __init__(self, cfg: dict, mode: str, path: str = CONFIG_PATH, sender=None):
        self.path = path
        self.mode = mode
        self._sender = sender or send_ntfy  # tests inject a fake
        self.sent = 0
        self.last = None                    # {"time", "title", "text", "ok", "error"}
        self.history: list[dict] = []       # recent sends, newest first
        self._state = "ok"                  # committed state: ok / hi / lo
        self._cand = None                   # (state, since_ms, extreme) waiting out hold_ms
        self._last_alert_wall = 0.0
        self._alerted_this_excursion = False
        self._peak = 0.0
        self._q: queue.Queue[dict] = queue.Queue()
        self.set_config(cfg)
        threading.Thread(target=self._worker, daemon=True).start()

    def set_config(self, cfg: dict) -> None:
        self.cfg = cfg
        p = problems(cfg)
        self.configured = not p
        self.problem = ", ".join(p)

    def update_config(self, upd: dict) -> None:
        new = apply_update(self.cfg, upd)   # raises ValueError on bad input
        save_config(new, self.path)
        self.set_config(new)

    @property
    def muted(self) -> bool:
        return not self.cfg["enabled"]

    # called from Hub.push under the hub lock - must stay fast
    def feed(self, t_ms: int, current: float, low: float, high: float) -> None:
        st = "hi" if current > high else "lo" if current < low else "ok"
        if st != "ok" and st == self._state:
            self._peak = max(self._peak, current) if st == "hi" else min(self._peak, current)
        if st == self._state:
            self._cand = None
            return
        if self._cand is None or self._cand[0] != st:
            self._cand = (st, t_ms, current)
        else:  # track the extreme while waiting out hold_ms
            c = self._cand[2]
            ext = max(c, current) if st == "hi" else min(c, current) if st == "lo" else current
            self._cand = (st, self._cand[1], ext)
        if t_ms - self._cand[1] < self.cfg["hold_ms"]:
            return

        prev, self._state = self._state, st
        cand_val, self._cand = self._cand[2], None
        if st != "ok":
            self._peak = cand_val
            self._alerted_this_excursion = False
            if time.time() - self._last_alert_wall >= self.cfg["cooldown_s"]:
                hi_ = st == "hi"
                limit = high if hi_ else low
                if self._enqueue({
                    "title": f"Current {'ABOVE HIGH' if hi_ else 'BELOW LOW'} limit: {current:.0f} mA{self._demo()}",
                    "text": f"{current:.0f} mA (limit {limit:.0f} mA)\n"
                            f"Allowed range: {low:.0f}–{high:.0f} mA\n{self._when()}",
                    "tags": ["warning", "arrow_up" if hi_ else "arrow_down"],
                    "priority": ALERT_PRIORITY,
                }):
                    self._alerted_this_excursion = True
        elif prev != "ok" and self._alerted_this_excursion and self.cfg["notify_recovery"]:
            self._enqueue({
                "title": f"Current back in range: {current:.0f} mA{self._demo()}",
                "text": f"Peak during excursion: {self._peak:.0f} mA\n"
                        f"Allowed range: {low:.0f}–{high:.0f} mA\n{self._when()}",
                "tags": ["white_check_mark"],
                "priority": RECOVERY_PRIORITY,
            })

    def _demo(self) -> str:
        return " (DEMO)" if self.mode == "demo" else ""

    @staticmethod
    def _when() -> str:
        return time.strftime("%d %b %H:%M:%S")

    def _enqueue(self, msg: dict) -> bool:
        if self.configured and not self.muted:
            self._last_alert_wall = time.time()
            self._q.put(msg)
            return True
        return False

    def send_test(self) -> None:
        self._q.put({"title": "Test from Current Monitor",
                     "text": f"Push alerts are working.\n{self._when()}",
                     "tags": ["bell"], "priority": 3})

    def _worker(self) -> None:
        while True:
            msg = self._q.get()
            entry = {"time": time.time(), "title": msg["title"], "text": msg["text"],
                     "ok": False, "error": "", "to": self.cfg["topic"]}
            try:
                self._sender(self.cfg, msg)
                entry["ok"] = True
                self.sent += 1
                print(f"[alert] ntfy/{self.cfg['topic']}: {msg['title']}")
            except Exception as e:  # network/provider errors must never kill the server
                entry["error"] = str(e)[:200] or e.__class__.__name__
                print(f"[alert] FAILED: {entry['error']}")
            self.last = entry
            self.history = [entry] + self.history[:19]
            time.sleep(1)  # space out bursts (ntfy.sh rate-limits)

    def status(self) -> dict:
        return {
            "configured": self.configured,
            "problem": self.problem,
            "server": self.cfg["server"],
            "topic": self.cfg["topic"],
            "muted": self.muted,
            "sent": self.sent,
            "last": self.last,
            "cooldown_s": self.cfg["cooldown_s"],
            "hold_ms": self.cfg["hold_ms"],
        }
