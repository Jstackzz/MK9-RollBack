"""Compare two games' script traces (MK9_TRACE_SCRIPT; -TraceScript in netplay-local.ps1) frame by frame.

Each game's trace is played the way its rollbacks played it: a "rollback to r" line takes back what
frames r and later did, and the frames run again ("r<frame>" lines) count instead. What is left is
what each frame did in the end: the script processes made and the random numbers scripts drew, in
order. Two games in step have the same, frame for frame; a frame that differs is listed (addresses
inside the heaps are left out: each PC's heap puts things elsewhere).
Usage: scriptcmp.py a/script.txt b/script.txt [first frame] [last frame]
"""
import re
import sys

LINE = re.compile(r"^(r?)(\d+) (\w+) (.*)$")


def matches(path):
    """per match, {frame: [what it did]} as it stands after the rollbacks"""
    out, cur, last = [], {}, -1
    for raw in open(path, encoding="ascii", errors="replace"):
        raw = raw.rstrip("\n")
        if raw.startswith("rollback to "):
            to = int(raw.split()[2])
            for f in [f for f in cur if f >= to]:
                del cur[f]
            continue
        m = LINE.match(raw)
        if not m:
            continue
        again, f, kind, rest = m.group(1) == "r", int(m.group(2)), m.group(3), m.group(4)
        if not again and f + 100 < last:  # the frames start again: the next match
            out.append(cur)
            cur = {}
        if not again:
            last = f
        rest = re.sub(r"\b(ctx|obj|ip) [0-9A-F]{8}\s*", "", rest)
        cur.setdefault(f, []).append(f"{kind} {rest}".strip())
    out.append(cur)
    return out


a, b = matches(sys.argv[1]), matches(sys.argv[2])
lo = int(sys.argv[3]) if len(sys.argv) > 3 else 0
hi = int(sys.argv[4]) if len(sys.argv) > 4 else 1 << 31
print(f"{len(a)} matches in A, {len(b)} in B")
for m, (ma, mb) in enumerate(zip(a, b), 1):
    # the frames both games ran (one may have stopped earlier)
    end = min(max(ma, default=0), max(mb, default=0))
    frames = [f for f in sorted(set(ma) | set(mb)) if lo <= f <= min(hi, end)]
    bad = [f for f in frames if ma.get(f, []) != mb.get(f, [])]
    events = sum(len(ma.get(f, [])) for f in frames)
    print(f"match {m}: {events} events in {len(frames)} frames up to {end}; {len(bad)} frames differ"
          + (f", first {bad[:8]}" if bad else ""))
    for f in bad[:6]:
        ea, eb = ma.get(f, []), mb.get(f, [])
        print(f"  frame {f}: A {len(ea)} events, B {len(eb)}")
        for x in [x for x in ea if x not in eb][:6]:
            print(f"    A only: {x}")
        for x in [x for x in eb if x not in ea][:6]:
            print(f"    B only: {x}")
