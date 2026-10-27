"""Find words in guest memory that say which screen the game is on: from dumps taken at known
screens (MK9_DIAG_DUMP, best restricted to the executable's data with MK9_DIAG_DUMP_RANGE), in one
or more runs, the 32-bit words that hold one value on a screen in every dump of it and a different
value in every dump of any other screen.
Usage: screenvars.py <run dir>:<frame>=<screen>,<frame>=<screen>,... [<run dir>:...] [--max 12]"""
import os
import struct
import sys
from collections import defaultdict

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


limit = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 12
dumps, labels = [], []
for arg in sys.argv[1:]:
    if ":" not in arg or arg.startswith("--"):
        continue
    run, spec = arg.rsplit(":", 1)
    for item in spec.split(","):
        frame, label = item.split("=")
        p = os.path.join(run, f"dump-{frame}.bin")
        if os.path.exists(p):
            dumps.append(load(p))
            labels.append(label)
screens = sorted(set(labels), key=labels.index)
common = set(dumps[0])
for d in dumps[1:]:
    common &= set(d)
found = defaultdict(list)
for base in sorted(common):
    n = min(len(d[base]) for d in dumps)
    m = np.stack([d[base][:n] for d in dumps])
    for s in screens:
        ins = [i for i, l in enumerate(labels) if l == s]
        outs = [i for i, l in enumerate(labels) if l != s]
        ok = np.ones(n, dtype=bool)
        for r in ins[1:]:
            ok &= m[r] == m[ins[0]]
        for r in outs:
            ok &= m[r] != m[ins[0]]
        for w in np.nonzero(ok)[0]:
            found[s].append((int(m[ins[0]][w]) >= 0x10000, base + 4 * int(w), int(m[ins[0]][w])))
for s in screens:
    hits = sorted(found[s])
    print(f"{s}: {len(hits)} words ({labels.count(s)} dumps)")
    for _, addr, v in hits[:limit]:
        print(f"  {addr:08X} = {v:X}")
