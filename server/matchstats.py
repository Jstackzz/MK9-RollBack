"""Match stats: what a match was like on each player's PC (docs/PROTOCOL.md, "Match stats").

A launcher sends them at the match's end, from what its game kept of the fight (mk9/src/mk9/net/netplay.h,
MatchStatsJson): how long each fight frame stayed on the screen, the rollbacks and how deep they went, the waits at the
prediction limit, the frames held back to keep pace with the other PC, the other's frame rate, and a line for every
second. The server keeps them beside the match (db.py, match_stats) and logs a line (STATS); `python -m server.matchlog`
shows them. Here: checking what came in, and the words for it.
"""

from __future__ import annotations

import math
from typing import Any

# fields kept, with their largest value; anything else in `stats` is dropped
INTS = {
    "v": 1000, "delay": 60, "frames": 10**8, "late": 10**8, "stalls": 10**8, "rollbacks": 10**8, "rerun": 10**9,
    "waits": 10**8, "held": 10**8, "step": 60, "threads": 4096,
}
FLOATS = {
    "secs": 10**7, "fight_secs": 10**7, "longest_ms": 10**8, "rollback_ms": 10**9, "rollback_longest_ms": 10**8,
    "wait_ms": 10**9, "their_fps": 1000, "my_fps": 1000, "round_trip": 10**5,
}
TEXTS = {"end": 16, "netcode": 16, "cpu": 100, "gpu": 100, "os": 100}
DEPTHS = 4  # rollbacks 1, 2, 3-4, 5+ frames deep
PER_SECOND_MAX = 1200  # lines kept (a message is at most protocol.MAX_LINE: about 1,000 fit)
PER_SECOND_FIELDS = 6  # fight frames, late ones, frames run again, the deepest rollback, ms waited, frames held back


def _int(v: Any, hi: int) -> int:
    if isinstance(v, bool) or not isinstance(v, int) or not 0 <= v <= hi:
        raise ValueError
    return v


def _float(v: Any, hi: float) -> float:
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) or not 0 <= v <= hi:
        raise ValueError
    return round(float(v), 2)


def _text(v: Any, n: int) -> str:
    if not isinstance(v, str):
        raise ValueError
    return "".join(c if " " <= c <= "~" else "?" for c in v[:n]).strip()


def clean(stats: Any) -> tuple[dict[str, Any], list[list[int]]]:
    """The summary and the lines a second of a `match_stats` message's `stats`, the fields known and checked
    (ValueError for a wrong one)."""
    if not isinstance(stats, dict):
        raise ValueError("stats must be an object")
    out: dict[str, Any] = {}
    try:
        for k, hi in INTS.items():
            if k in stats:
                out[k] = _int(stats[k], hi)
        for k, hi in FLOATS.items():
            if k in stats:
                out[k] = _float(stats[k], hi)
        for k, n in TEXTS.items():
            if k in stats:
                out[k] = _text(stats[k], n)
        if "depth" in stats:
            d = stats["depth"]
            if not isinstance(d, list) or len(d) != DEPTHS:
                raise ValueError
            out["depth"] = [_int(x, 10**8) for x in d]
        lines: list[list[int]] = []
        if "per_second" in stats:
            ps = stats["per_second"]
            if not isinstance(ps, list) or len(ps) > PER_SECOND_MAX:
                raise ValueError
            for line in ps:
                if not isinstance(line, list) or len(line) != PER_SECOND_FIELDS:
                    raise ValueError
                lines.append([_int(x, 65535) for x in line])
    except ValueError:
        raise ValueError("stats fields out of range or of the wrong type") from None
    return out, lines


def rates(st: dict[str, Any]) -> dict[str, float]:
    """Per minute of fight (late frames, stalls, rollbacks 3 or more frames deep) and the fight's frame rate; empty
    without a fight of 5 s or more."""
    secs = st.get("fight_secs") or 0
    if secs < 5:
        return {}
    minutes = secs / 60
    depth = st.get("depth") or [0] * DEPTHS
    return {
        "fps": st.get("frames", 0) / secs,
        "late_pm": st.get("late", 0) / minutes,
        "stalls_pm": st.get("stalls", 0) / minutes,
        "deep_pm": sum(depth[2:]) / minutes,
        "rollbacks_ps": st.get("rollbacks", 0) / secs,
    }


def verdict(st: dict[str, Any]) -> str:
    """How smooth the fight looked on this PC: smooth, some stutter, stutter (or no fight). From what the screen
    showed: the frame rate, frames shown again (over 25 ms) and stalls (over 50 ms) a minute, the time stood still at the
    prediction limit. Rollbacks by themselves are not stutter (a deep one is a jump, shown apart)."""
    r = rates(st)
    if not r:
        return "no fight"
    wait_ms = st.get("wait_ms", 0)
    if r["fps"] < 50 or r["late_pm"] >= 30 or r["stalls_pm"] >= 10 or wait_ms >= 3000:
        return "stutter"
    if r["fps"] < 57 or r["late_pm"] >= 6 or r["stalls_pm"] >= 2 or wait_ms >= 500:
        return "some stutter"
    return "smooth"


def clock(secs: float) -> str:
    s = int(round(secs))
    return f"{s // 60}:{s % 60:02d}"


def summary(st: dict[str, Any]) -> str:
    """One line for the log and the match log."""
    r = rates(st)
    depth = st.get("depth") or [0] * DEPTHS
    parts = [verdict(st)]
    if r:
        parts.append(f"{r['fps']:.1f} fps over {clock(st.get('fight_secs', 0))} of fight, "
                     f"{st.get('late', 0)} frames late ({r['late_pm']:.1f} a minute, longest "
                     f"{st.get('longest_ms', 0):.0f} ms), {st.get('rollbacks', 0)} rollbacks ({r['rollbacks_ps']:.1f} a "
                     f"second, {sum(depth[2:])} of 3+ frames), {st.get('waits', 0)} waits at the limit "
                     f"({st.get('wait_ms', 0):.0f} ms), {st.get('held', 0)} frames held back")
        if st.get("their_fps"):
            parts.append(f"the other at {st['their_fps']:.0f} fps, this PC at {st.get('my_fps', 0):.0f}")
    trip = f"round trip {st['round_trip']:.1f} frames, " if st.get("round_trip") else ""
    parts.append(f"{trip}delay {st.get('delay', '?')}, {st.get('netcode', '?')}, ended {st.get('end', '?')}")
    return "; ".join(parts)
