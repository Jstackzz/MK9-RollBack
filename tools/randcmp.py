"""Compare two games' random number traces (MK9_TRACE_RAND): where the draw sequences part.
Lines: <netplay frame> <boot frame> <kind> <caller> <value> <thread m|o>.
Usage: randcmp.py a.txt b.txt [--kinds GFS] [--context 8] [--from-match-frame N]"""
import sys
from collections import Counter


def opt(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


ctxn = int(opt("--context", 8))
kinds = set(opt("--kinds", "GFS"))
start = int(opt("--from-match-frame", 0))


def load(path):
    out = []
    for line in open(path):
        x = line.split()
        if len(x) >= 6 and x[2] in kinds and (start == 0 or int(x[0]) >= start):
            out.append(x)
    return out


a, b = load(sys.argv[1]), load(sys.argv[2])
print(f"{len(a)} draws in A, {len(b)} in B (kinds {''.join(sorted(kinds))})")
# compare kind, caller, value and thread, not the frame numbers (a barrier may offset them)
for i, (x, y) in enumerate(zip(a, b)):
    if x[2:] != y[2:]:
        print(f"first difference at draw {i}:")
        for k in range(max(0, i - ctxn), min(i + ctxn, len(a), len(b))):
            mark = "<--" if k == i else ""
            print(f"  A {' '.join(a[k]):40} | B {' '.join(b[k]):40} {mark}")
        break
else:
    print("same for the common length")
ca, cb = Counter((x[2], x[3], x[5]) for x in a), Counter((y[2], y[3], y[5]) for y in b)
print("callers (kind caller thread: A count / B count):")
for c in sorted(set(ca) | set(cb), key=lambda c: -(ca[c] + cb[c]))[:30]:
    print(f"  {' '.join(c)}: {ca[c]} / {cb[c]}")
