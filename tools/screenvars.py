"""Find words in guest memory that say which screen the game is on: from dumps taken at known
screens (MK9_DIAG_DUMP, best restricted to the executable's data with MK9_DIAG_DUMP_RANGE), the
32-bit words that are equal across dumps of the same screen and differ between screens.
Usage: screenvars.py <dump dir> 1700=title 2000=main 2400=main 3000=select ... [--max 40]"""
import os
import struct
import sys

import numpy as np


def load(path):
    out = {}
    with open(path, "rb") as f:
        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                break
            addr, size = struct.unpack("<II", hdr)
            out[addr] = np.frombuffer(f.read(size), dtype=">u4")
    return out


args = [a for a in sys.argv[2:] if "=" in a]
limit = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 40
frames = [(int(a.split("=")[0]), a.split("=")[1]) for a in args]
dumps = [load(os.path.join(sys.argv[1], f"dump-{f}.bin")) for f, _ in frames]
labels = [s for _, s in frames]
screens = sorted(set(labels), key=labels.index)

# the regions every dump has, word by word
common = set(dumps[0])
for d in dumps[1:]:
    common &= set(d)
hits = []
for base in sorted(common):
    n = min(len(d[base]) for d in dumps)
    m = np.stack([d[base][:n] for d in dumps])  # dumps x words
    ok = np.ones(n, dtype=bool)
    # equal within each screen
    for s in screens:
        rows = [i for i, l in enumerate(labels) if l == s]
        for r in rows[1:]:
            ok &= m[r] == m[rows[0]]
    # different between every pair of screens
    firsts = [labels.index(s) for s in screens]
    for i in range(len(firsts)):
        for j in range(i + 1, len(firsts)):
            ok &= m[firsts[i]] != m[firsts[j]]
    for w in np.nonzero(ok)[0]:
        vals = [int(m[k][w]) for k in firsts]
        small = all(v < 0x10000 for v in vals)
        hits.append((not small, base + 4 * int(w), vals))
hits.sort()
print(f"{len(hits)} words tell the screens {', '.join(screens)} apart")
for _, addr, vals in hits[:limit]:
    print(f"  {addr:08X}: " + "  ".join(f"{s}={v:X}" for s, v in zip(screens, vals)))
