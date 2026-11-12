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
    # gaps between successive frames' presents over 25 ms and over 50 ms (game 0.5.11 and up): what the screen got
    "shown_late": 10**8, "shown_stalls": 10**8,
}
FLOATS = {
    "secs": 10**7, "fight_secs": 10**7, "longest_ms": 10**8, "rollback_ms": 10**9, "rollback_longest_ms": 10**8,
    "wait_ms": 10**9, "their_fps": 1000, "my_fps": 1000, "round_trip": 10**5,
}
TEXTS = {"end": 16, "netcode": 16, "cpu": 100, "gpu": 100, "os": 100}
# where the main thread stood in late frames (game 0.5.11 and up): the most common looks, "41% <host code and callers> |
# main <guest return addresses> | render <the rendering thread's>"
STALL_WHERE_MAX = 8
STALL_WHERE_CHARS = 260
DEPTHS = 4  # rollbacks 1, 2, 3-4, 5+ frames deep
# the late frames by what made them long (game 0.5.11 and up): a hold, a wait at the prediction limit, a rollback, the main
# thread's own work, the main thread blocked on something else
LATE_WHY = ("held", "waited", "rollback", "own work", "blocked")
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
        if "stall_where" in stats:
            sw = stats["stall_where"]
            if not isinstance(sw, list) or len(sw) > STALL_WHERE_MAX:
                raise ValueError
            out["stall_where"] = [_text(x, STALL_WHERE_CHARS) for x in sw]
        if "late_why" in stats:
            d = stats["late_why"]
            if not isinstance(d, list) or len(d) != len(LATE_WHY):
                raise ValueError
            out["late_why"] = [_int(x, 10**8) for x in d]
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
    # from game 0.5.11 what the screen got (the presenter's gaps), else the frames' own tops: a PC that makes up a late
    # frame by running the next ones close together has long frames after them that the screen never shows
    late = st["shown_late"] if "shown_late" in st else st.get("late", 0)
    stalls = st["shown_stalls"] if "shown_stalls" in st else st.get("stalls", 0)
    return {
        "fps": st.get("frames", 0) / secs,
        "late_pm": late / minutes,
        "stalls_pm": stalls / minutes,
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
        if "shown_late" in st:
            parts.append(f"the screen: {st['shown_late']} gaps over 25 ms, {st.get('shown_stalls', 0)} over 50")
        if st.get("late_why") and st.get("late"):
            parts.append("late frames: " + ", ".join(f"{n} {w}" for w, n in zip(LATE_WHY, st["late_why"]) if n))
        if st.get("their_fps"):
            parts.append(f"the other at {st['their_fps']:.0f} fps, this PC at {st.get('my_fps', 0):.0f}")
    trip = f"round trip {st['round_trip']:.1f} frames, " if st.get("round_trip") else ""
    parts.append(f"{trip}delay {st.get('delay', '?')}, {st.get('netcode', '?')}, ended {st.get('end', '?')}")
    return "; ".join(parts)
