"""The match log as a web page (python -m server.matchlog --html): every match a line, open it for each player's PC,
second by second. One file, nothing loaded from elsewhere."""

from __future__ import annotations

import html
import time
from typing import Any

from . import matchstats

STYLE = """
:root{--bg:#f6f6f3;--card:#fff;--ink:#1c1c1e;--muted:#6a6a70;--line:#e2e2de;--ok:#1d8048;--some:#a86b12;--bad:#c23a2e;
--none:#8a8a90;--fps:#2f68d8;--late:#d43d3d;--wait:#e0951f;--deep:#9a9aa4}
@media (prefers-color-scheme:dark){:root{--bg:#141416;--card:#1e1e21;--ink:#ececef;--muted:#9c9ca4;--line:#2f2f34;
--ok:#46c27a;--some:#e3a447;--bad:#ef6a5d;--none:#77777f;--fps:#6f9cf5;--late:#f06464;--wait:#f0b14a;--deep:#6d6d78}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.45 system-ui,-apple-system,
"Segoe UI",sans-serif}header,main{max-width:1100px;margin:0 auto;padding:16px}h1{font-size:22px;margin:8px 0 2px}
.sub{color:var(--muted);margin:0 0 10px}.counts{display:flex;gap:8px;flex-wrap:wrap}
.pill{display:inline-block;border-radius:999px;padding:1px 9px;font-size:12px;font-weight:600;color:#fff;white-space:nowrap}
.smooth{background:var(--ok)}.some{background:var(--some)}.stutter{background:var(--bad)}.nofight,.nostats{background:var(--none)}
details{background:var(--card);border:1px solid var(--line);border-radius:10px;margin:8px 0}
summary{cursor:pointer;padding:10px 12px;display:grid;grid-template-columns:92px 1fr auto;gap:4px 12px;align-items:center}
summary::-webkit-details-marker{display:none}.when{color:var(--muted);font-variant-numeric:tabular-nums}
.who{font-weight:600}.meta{color:var(--muted);font-size:12px}.pills{display:flex;gap:6px;flex-wrap:wrap;justify-content:end}
.sides{padding:0 12px 12px;display:grid;gap:12px}.side{border-top:1px solid var(--line);padding-top:10px}
.side h3{margin:0 0 4px;font-size:14px;display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.nums{display:flex;flex-wrap:wrap;gap:4px 14px;color:var(--muted);font-size:12px;font-variant-numeric:tabular-nums}
.nums b{color:var(--ink);font-weight:600}svg{display:block;width:100%;height:auto;margin-top:6px}
.legend{display:flex;gap:12px;flex-wrap:wrap;font-size:12px;color:var(--muted)}.key{display:inline-block;width:10px;
height:10px;border-radius:2px;margin-right:4px;vertical-align:-1px}.hw{font-size:12px;color:var(--muted)}
@media (max-width:640px){summary{grid-template-columns:1fr}.pills{justify-content:start}}
"""

CLASS = {"smooth": "smooth", "some stutter": "some", "stutter": "stutter", "no fight": "nofight"}


def pill(text: str, cls: str) -> str:
    return f'<span class="pill {cls}">{html.escape(text)}</span>'


def timeline(lines: list[list[int]], step: int = 1) -> str:
    """Each second: the fight frames shown (the line, 0 to 60), frames shown again (red, from the top), ms waited at
    the prediction limit (orange), a rollback 3 or more frames deep (grey, at the bottom)."""
    n = len(lines)
    if not n or not any(line[0] for line in lines):
        return ""
    w, h, top, bottom = 900, 104, 18, 72
    dx = w / n
    pts, marks = [], []
    for i, (frames, late, _again, deep, wait, _held) in enumerate(lines):
        x = i * dx
        if frames:
            fps = min(60.0, frames / step)
            pts.append(f"{x + dx / 2:.1f},{bottom - fps / 60 * (bottom - top):.1f}")
        elif pts:
            marks.append(f'<polyline points="{" ".join(pts)}" fill="none" stroke="var(--fps)" stroke-width="1.6"/>')
            pts = []
        bw = max(1.0, dx - 0.5)
        if late:
            marks.append(f'<rect x="{x:.1f}" y="0" width="{bw:.1f}" height="{min(16.0, 2.0 + late * 2):.1f}" '
                         f'fill="var(--late)"><title>{i * step}s: {late} frames shown again</title></rect>')
        if wait:
            marks.append(f'<rect x="{x:.1f}" y="{top - 2}" width="{bw:.1f}" height="{min(14.0, 2.0 + wait / 20):.1f}" '
                         f'fill="var(--wait)" opacity=".85"><title>{i * step}s: {wait} ms waiting</title></rect>')
        if deep >= 3:
            marks.append(f'<rect x="{x:.1f}" y="{bottom + 3}" width="{bw:.1f}" height="{min(12.0, deep * 1.5):.1f}" '
                         f'fill="var(--deep)"><title>{i * step}s: a rollback {deep} frames deep</title></rect>')
    if pts:
        marks.append(f'<polyline points="{" ".join(pts)}" fill="none" stroke="var(--fps)" stroke-width="1.6"/>')
    grid = "".join(f'<line x1="0" x2="{w}" y1="{y:.1f}" y2="{y:.1f}" stroke="var(--line)" stroke-width="1"/>'
                   for y in (top, (top + bottom) / 2, bottom))
    labels = "".join(f'<text x="{(k * 60 / step) * dx:.1f}" y="{h - 2}" font-size="10" fill="var(--muted)">'
                     f"{k}:00</text>" for k in range(0, int(n * step / 60) + 1))
    return (f'<svg viewBox="0 0 {w} {h + 2}" role="img" aria-label="frames a second through the match">'
            f"{grid}{''.join(marks)}{labels}</svg>")


def side_html(name: str, side: dict[str, Any] | None) -> str:
    if side is None:
        return (f'<div class="side"><h3>{html.escape(name)} {pill("no stats", "nostats")}</h3>'
                f'<div class="hw">A launcher before 0.5.9, or none sent.</div></div>')
    st = side["stats"]
    v = matchstats.verdict(st)
    r = matchstats.rates(st)
    nums = []
    if r:
        depth = st.get("depth") or [0, 0, 0, 0]
        nums = [
            f"<b>{r['fps']:.1f}</b> fps",
            f"<b>{st.get('late', 0)}</b> frames shown again ({r['late_pm']:.1f}/min, longest {st.get('longest_ms', 0):.0f} ms)",
            f"<b>{st.get('stalls', 0)}</b> over 50 ms",
            f"<b>{st.get('rollbacks', 0)}</b> rollbacks ({r['rollbacks_ps']:.1f}/s; 1/2/3-4/5+ frames: "
            f"{'/'.join(str(d) for d in depth)})",
            f"<b>{st.get('waits', 0)}</b> waits ({st.get('wait_ms', 0):.0f} ms)",
            f"<b>{st.get('held', 0)}</b> frames held back",
        ]
        if st.get("their_fps"):
            nums.append(f"other PC <b>{st['their_fps']:.0f}</b> fps, this one {st.get('my_fps', 0):.0f}")
    nums.append(f"fight {matchstats.clock(st.get('fight_secs', 0))}")
    if st.get("round_trip"):
        nums.append(f"round trip {st['round_trip']:.1f} frames")
    nums.append(f"delay {st.get('delay', '?')}, {html.escape(str(st.get('netcode', '?')))}, "
                f"ended {html.escape(str(st.get('end', '?')))}")
    hw = " · ".join(html.escape(str(st[k])) for k in ("cpu", "gpu", "os") if st.get(k))
    launcher = f" · launcher {html.escape(side['launcher'])}" if side.get("launcher") else ""
    return (f'<div class="side"><h3>{html.escape(name)} {pill(v, CLASS.get(v, "nofight"))}</h3>'
            f'<div class="nums">{"".join(f"<span>{n}</span>" for n in nums)}</div>'
            f'{timeline(side.get("per_second") or [], int(st.get("step", 1)))}'
            f'<div class="hw">{hw}{launcher}</div></div>')


def match_html(m: dict[str, Any], ending: str) -> str:
    kind = f"{m['mode']} {'ranked' if m['ranked'] else 'casual'}" + (" lobby" if m["lobby"] else "")
    rtt = f"{m['rtt_ms']} ms" if m["rtt_ms"] is not None else "-"
    pills = []
    for side, name in ((1, m["p1_name"]), (2, m["p2_name"])):
        s = m["sides"].get(side)
        v = matchstats.verdict(s["stats"]) if s else "no stats"
        pills.append(pill(f"{name}: {v}", CLASS.get(v, "nostats")))
    sides = "".join(side_html(name, m["sides"].get(side)) for side, name in ((1, m["p1_name"]), (2, m["p2_name"])))
    return (f'<details><summary><span class="when">{time.strftime("%m-%d %H:%M", time.gmtime(m["started"]))}</span>'
            f'<span><span class="who">{html.escape(m["p1_name"] or "?")} vs {html.escape(m["p2_name"] or "?")}</span><br>'
            f'<span class="meta">{html.escape(kind)} · round trip {rtt} · {html.escape(ending)} · {m["id"]}</span></span>'
            f'<span class="pills">{"".join(pills)}</span></summary><div class="sides">{sides}</div></details>')


def page(matches: list[dict[str, Any]], since: float, ending) -> str:
    counts: dict[str, int] = {}
    for m in matches:
        for s in m["sides"].values():
            v = matchstats.verdict(s["stats"])
            counts[v] = counts.get(v, 0) + 1
    count_pills = "".join(pill(f"{counts[v]} {v}", CLASS[v]) for v in ("smooth", "some stutter", "stutter", "no fight")
                          if counts.get(v))
    body = "".join(match_html(m, ending(m)) for m in reversed(matches)) or "<p>No matches.</p>"
    legend = ('<div class="legend"><span><i class="key" style="background:var(--fps)"></i>frames a second</span>'
              '<span><i class="key" style="background:var(--late)"></i>frames shown again</span>'
              '<span><i class="key" style="background:var(--wait)"></i>waiting for the other PC</span>'
              '<span><i class="key" style="background:var(--deep)"></i>rollback 3+ frames deep</span></div>')
    return ("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
            "content=\"width=device-width,initial-scale=1\"><title>MK9 Match Log</title>"
            f"<style>{STYLE}</style></head><body><header><h1>Match log</h1>"
            f'<p class="sub">{len(matches)} matches since {time.strftime("%Y-%m-%d %H:%M", time.gmtime(since))} UTC, '
            f'newest first · made {time.strftime("%Y-%m-%d %H:%M", time.gmtime())} UTC</p>'
            f'<div class="counts">{count_pills}</div></header><main>{legend}{body}</main></body></html>')
