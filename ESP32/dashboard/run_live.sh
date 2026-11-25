#!/usr/bin/env bash
# Option 1: live mode - reads the Arduino over USB serial.
#   ./run_live.sh                      auto-picks the first /dev/ttyACM* or /dev/ttyUSB*
#   ./run_live.sh /dev/ttyUSB0 --baud 9600 --low 250 --high 750
cd "$(dirname "$0")"
PORT="$1"
if [ -z "$PORT" ] || [[ "$PORT" == --* ]]; then
  PORT=$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -n1)
  [ -n "$PORT" ] || { echo "No Arduino found on /dev/ttyACM* or /dev/ttyUSB*. Pass the port explicitly."; exit 1; }
  echo "Using $PORT"
else
  shift
fi
exec python3 server.py --serial "$PORT" "$@"
