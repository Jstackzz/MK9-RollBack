"""The match log: the matches lately, each with what it was like on both players' PCs (matchstats.py).

    python -m server.matchlog --db /var/lib/mk9/server.sqlite                last 24 hours
    python -m server.matchlog --db ... --hours 6 --player Name               one player's
    python -m server.matchlog --db ... --match 5a9e1f5b7a69                  one match, second by second
    python -m server.matchlog --db ... --json                                everything as JSON
    python -m server.matchlog --db ... --html > log.html                     a page (matchpage.py; tools/matchlog.py)

Read only (the database opened with mode=ro). Times are UTC. A match's line: when, its id, mode, players, the round
trip the server measured, how it ended; then a line a player: how smooth the fight was on their PC (matchstats.verdict),
its frame rate, frames shown again a minute (and the longest), rollbacks a second and those 3 or more frames deep a
minute, waits at the prediction limit, frames held back to keep pace, and the other's frame rate as this PC saw it.
"""

from __future__ import annotations

import argparse
import json
import sqlite3
import sys
import time
from typing import Any

from . import matchpage, matchstats


def connect(path: str) -> sqlite3.Connection:
    conn = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    conn.row_factory = sqlite3.Row
    return conn


def load(conn: sqlite3.Connection, *, since: float, player: str | None = None, match: str | None = None,
         limit: int = 500) -> list[dict[str, Any]]:
    """The matches that started since `since` (or the one `match`), oldest first, with each side's stats."""
    has_stats = conn.execute(
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'match_stats'").fetchone() is not None
    where, args = ["m.started IS NOT NULL"], []
    if match:
        where.append("m.id = ?")
        args.append(match)
    else:
        where.append("m.created >= ?")
        args.append(since)
    if player:
        where.append("(lower(a1.name) = lower(?) OR lower(a2.name) = lower(?))")
        args += [player, player]
    rows = conn.execute(
        "SELECT m.*, a1.name AS p1_name, a2.name AS p2_name FROM matches m "
        "LEFT JOIN accounts a1 ON a1.id = m.p1 LEFT JOIN accounts a2 ON a2.id = m.p2 "
        f"WHERE {' AND '.join(where)} ORDER BY m.created DESC LIMIT ?", (*args, limit)).fetchall()
    out = []
    for r in reversed(rows):
        m = {k: r[k] for k in r.keys()}
        m["sides"] = {}
        if has_stats:
            for s in conn.execute("SELECT * FROM match_stats WHERE match_id = ? ORDER BY side", (r["id"],)):
                m["sides"][s["side"]] = {
                    "launcher": s["launcher"], "received": s["received"], "stats": json.loads(s["stats"]),
                    "per_second": json.loads(s["per_second"]) if s["per_second"] else [],
                }
        out.append(m)
    return out


def when(t: float | None, fmt: str = "%Y-%m-%d %H:%M") -> str:
    return time.strftime(fmt, time.gmtime(t)) if t else "-"


def ending(m: dict[str, Any]) -> str:
    st = m["status"]
    if st == "agreed" and m["winner"] in (1, 2):
        name = m["p1_name"] if m["winner"] == 1 else m["p2_name"]
        return f"{name} won {m['p1_rounds']}-{m['p2_rounds']}"
    return st.replace("_", " ")


def side_line(name: str, side: dict[str, Any] | None) -> str:
    if side is None:
        return f"   {name:<16} no stats (a launcher before 0.5.9, or none sent)"
    st = side["stats"]
    r = matchstats.rates(st)
    v = matchstats.verdict(st)
    if not r:
        return f"   {name:<16} {v:<13} ended {st.get('end', '?')}"
    other = f"   other {st['their_fps']:.0f} fps" if st.get("their_fps") else ""
    return (f"   {name:<16} {v:<13} {r['fps']:5.1f} fps  {r['late_pm']:5.1f} late/min ({st.get('longest_ms', 0):.0f} ms)"
            f"  {r['rollbacks_ps']:4.1f} rb/s, {r['deep_pm']:4.1f} deep/min  waits {st.get('waits', 0)}"
            f" ({st.get('wait_ms', 0):.0f} ms)  held {st.get('held', 0)}{other}")


def match_lines(m: dict[str, Any]) -> list[str]:
    kind = f"{m['mode']} {'ranked' if m['ranked'] else 'casual'}" + (" lobby" if m["lobby"] else "")
    rtt = f"{m['rtt_ms']} ms" if m["rtt_ms"] is not None else "-"
    out = [f"{when(m['started'], '%m-%d %H:%M')}  {m['id']}  {kind:<16} {m['p1_name']} vs {m['p2_name']}"
           f"   round trip {rtt}   {ending(m)}"]
    for side, name in ((1, m["p1_name"]), (2, m["p2_name"])):
        out.append(side_line(name, m["sides"].get(side)))
    return out


def seconds_table(m: dict[str, Any]) -> list[str]:
    """The seconds of the match where something happened on either PC: fewer than 55 fight frames, a frame shown again,
    a rollback 3 or more frames deep, a wait at the limit, frames held back."""
    names = [m["p1_name"], m["p2_name"]]
    lines = [m["sides"].get(1, {}).get("per_second", []), m["sides"].get(2, {}).get("per_second", [])]
    n = max(len(lines[0]), len(lines[1]))
    out = [f"{'':6}" + "".join(f"{name[:30]:<34}" for name in names),
           f"{'time':6}" + "fps late again deep wait held      " * 2]

    def cell(line: list[int] | None) -> str:
        if not line:
            return f"{'':34}"
        frames, late, again, deep, wait, held = line
        return f"{frames:3} {late:4} {again:5} {deep:4} {wait:4} {held:4}      "

    def notable(line: list[int] | None) -> bool:
        if not line or not line[0]:
            return False
        frames, late, _again, deep, wait, held = line
        return frames < 55 or late > 0 or deep >= 3 or wait > 0 or held > 0

    shown = 0
    fight = 0
    for i in range(n):
        a = lines[0][i] if i < len(lines[0]) else None
        b = lines[1][i] if i < len(lines[1]) else None
        if (a and a[0]) or (b and b[0]):
            fight += 1
        if notable(a) or notable(b):
            out.append(f"{i // 60}:{i % 60:02d}  " + cell(a) + cell(b))
            shown += 1
    out.append(f"{shown} of {fight} seconds with fight frames had something (fps: fight frames that second; late: shown"
               f" again; again: frames run again; deep: the deepest rollback; wait: ms at the limit; held: frames held"
               f" back)")
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m server.matchlog", description=__doc__.split("\n\n")[0])
    ap.add_argument("--db", required=True, help="the server's database")
    ap.add_argument("--hours", type=float, default=24.0, help="matches that started in the last this many hours")
    ap.add_argument("--player", default=None, help="only this account's matches")
    ap.add_argument("--match", default=None, help="one match, with its seconds where something happened")
    ap.add_argument("--limit", type=int, default=500, help="the latest this many matches at most")
    ap.add_argument("--json", action="store_true", help="everything as JSON")
    ap.add_argument("--html", action="store_true", help="a page: every match, each player's PC second by second")
    args = ap.parse_args(argv)
    since = time.time() - args.hours * 3600
    with connect(args.db) as conn:
        matches = load(conn, since=since, player=args.player, match=args.match, limit=args.limit)
    if args.html:
        sys.stdout.buffer.write(matchpage.page(matches, since, ending).encode("utf-8"))
        sys.stdout.flush()
        return 0
    if args.json:
        json.dump({"since": since, "now": time.time(), "matches": matches}, sys.stdout, separators=(",", ":"))
        return 0
    if args.match and not matches:
        print(f"no match {args.match}")
        return 1
    both = sum(1 for m in matches if len(m["sides"]) == 2)
    counts: dict[str, int] = {}
    for m in matches:
        for side in m["sides"].values():
            v = matchstats.verdict(side["stats"])
            counts[v] = counts.get(v, 0) + 1
    if not args.match:
        print(f"{len(matches)} matches since {when(since)} UTC, {both} with stats from both players"
              + (f" ({', '.join(f'{n} {v}' for v, n in sorted(counts.items()))} on players' PCs)" if counts else ""))
        print()
    for m in matches:
        print("\n".join(match_lines(m)))
        if args.match:
            for side, name in ((1, m["p1_name"]), (2, m["p2_name"])):
                if side in m["sides"]:
                    st = m["sides"][side]["stats"]
                    print(f"   {name}: {matchstats.summary(st)}"
                          + (f"; {st['cpu']}" if st.get("cpu") else "") + (f"; {st['gpu']}" if st.get("gpu") else ""))
            print()
            print("\n".join(seconds_table(m)))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
