#!/usr/bin/env bash
# Option 2: demo mode - replays a ~3 min recording (loops). Extra args pass through,
# e.g.  ./run_demo.sh --speed 2 --low 250 --high 750
cd "$(dirname "$0")"
[ -f data/demo_current.csv ] || python3 make_demo_data.py
exec python3 server.py --demo "$@"
