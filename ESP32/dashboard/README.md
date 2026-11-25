# Current dashboard

A live graph of current against time, with the threshold band and the live value.
Open **http://localhost:8000** after starting one of the two options below.

## Option 1: live data from the Arduino

1. Flash `arduino/telemetry_serial/telemetry_serial.ino`, or copy `print_telemetry()` into your own sketch.
   That Arduino must print one line per sample: `timestamp_ms,current_ma,seq`.
   The SPI receiver's debug line also works as is:
   `[SPI RX] seq=N  node_t=<us> us  I=<uA> uA (<mA> mA) ...`. Records damaged by dropped serial
   characters are thrown away. The uA and mA values must agree, and node_t must step in line with seq.
2. Close the Arduino IDE Serial Monitor. Only one program can hold the serial port at a time.
3. Run:
   ```bash
   ./run_live.sh                      # auto-detects /dev/ttyACM* or /dev/ttyUSB*
   ./run_live.sh /dev/ttyUSB0 --baud 115200 --low 200 --high 800
   ```
   If you get a permission error on Linux, run `sudo usermod -aG dialout $USER`, then log out and back in.

## Option 2: demo from a file (~3 min, loops)

```bash
./run_demo.sh                 # real time
./run_demo.sh --speed 4       # 4x faster, to check everything quickly
```

`data/demo_current.csv` is created by `make_demo_data.py`. You can replay any CSV with
`timestamp_ms,current_ma,seq` columns: `python3 server.py --demo path/to/file.csv`.

## Changing the threshold

Type new **Low** / **High** values in the *Threshold range* card and press **Apply** (or Enter).
Press Esc to throw away your edit. The value is saved on the server, so all open tabs and the
push alerts use it.

## Push alerts (ntfy)

Alerts go to your phone as push notifications through [ntfy](https://ntfy.sh). It is free and
needs no account. The Python server sends them, so they work even with no dashboard tab open.

1. Install the **ntfy** app (Play Store, App Store or F-Droid).
2. In the dashboard, click **☰** (top left) or the **Alerts** pill (top right).
3. Press **Generate** to get a random topic, then **Save**.
4. In the ntfy app, tap **+** and subscribe to that topic. The drawer shows the topic and a link you can copy.
5. Press **Send test notification**.

Anyone who knows the topic can read it, so keep it hard to guess. Everyone subscribed gets the
alerts, so a whole team can share one topic.

*Advanced* in the drawer lets you set a self-hosted ntfy server URL and an access token (for protected
topics). The token is never sent back to the browser. Settings are saved to `alerts.json`
(git-ignored, file permissions 600).

**When a notification is sent**
- The current stays outside the range for longer than *Alert after* (default 50 ms).
- At most one alert per *Minimum gap* (default 10 s). The gap is shared by above-high and below-low.
- Alerts are sent with ntfy "high" priority; back-in-range notifications use "default".
- Optionally, a "back in range" notification that includes the peak value.
- The on/off switch takes effect immediately. Demo-mode alerts are marked `(DEMO)`.

## Options

| flag | default | |
|---|---|---|
| `--low` / `--high` | 200 / 800 | threshold range in mA (you can also edit it in the page) |
| `--port` | 8000 | HTTP port |
| `--baud` | 115200 | live mode only |
| `--speed`, `--no-loop` | 1, loops | demo mode only |
| `--alerts` | `alerts.json` | push-alert (ntfy) settings file |

## Files

```
server.py          HTTP server, SSE stream, threshold API
sources.py         SerialSource (live) and ReplaySource (demo), plus the line parser
static/index.html  the dashboard (no external dependencies, works offline)
alerts.py          alert rules + ntfy sender
make_demo_data.py  writes data/demo_current.csv
arduino/           helper sketch for the serial output
```

Requirements: Python 3.9+ (alerts need no extra packages). Live mode also needs `pyserial` (`pip install pyserial`).
