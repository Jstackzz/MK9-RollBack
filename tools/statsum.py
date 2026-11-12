#!/usr/bin/env python3
"""The match stats two games wrote (tools\\pace-local.ps1 keeps them as <name>.<a|b>.stats-<match>.json) as a table, one
line a game: what the SCREEN got (`scr25` / `scr50`: gaps between successive frames' presents over 25 ms and over 50 ms,
the numbers the server rates a match by since 0.5.11) next to the frame tops' own count (`late`), the late tops by cause
(`why`: held, waited at the prediction limit, rollback, the main thread's own work, blocked), the rollbacks and how
deep, the holds, and `lag`: how many frames the screen is behind the frame tops against the fight's first frame.

    python -I tools/statsum.py <results folder> [run name ...]
"""

from __future__ import annotations

import glob
import json
import os
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    folder, names = sys.argv[1], sys.argv[2:]
    print(f"{'run':16}{'':2}{'fps':>5}{'fight':>6}{'late':>5}{'scr25':>6}{'scr50':>6}{'rb':>5}{'rb/s':>5}{'depth 1/2/3-4/5+':>18}"
          f"{'held':>5}{'wait':>5}{'lag':>6}  why[held,wait,rb,own,blocked]")
    for f in sorted(glob.glob(os.path.join(folder, "*.stats-*.json"))):
        base = os.path.basename(f)
        run, side = base.split(".")[0], base.split(".")[1]
        if names and run not in names:
            continue
        try:
            with open(f, encoding="utf-8") as fh:
                d = json.load(fh)
        except (OSError, ValueError) as e:
            print(base, "unreadable", e)
            continue
        s = d.get("stats", d)
        secs = max(s.get("fight_secs", 1), 1)
        dep = s.get("depth", [0, 0, 0, 0])
        print(f"{run:16}{side:>2}{s.get('my_fps', 0):7.1f}{secs:6.0f}{s.get('late', 0):5}{s.get('shown_late', -1):6}"
              f"{s.get('shown_stalls', -1):6}{s.get('rollbacks', 0):5}{s.get('rollbacks', 0) / secs:5.2f}"
              f"{'/'.join(str(x) for x in dep):>18}{s.get('held', 0):5}{s.get('waits', 0):5}"
              f"{s.get('screen_lag', float('nan')):6.2f}  {s.get('late_why', '')}")
        for line in s.get("stall_where", [])[:3]:
            print(f"{'':18}late frames, the main thread at {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
