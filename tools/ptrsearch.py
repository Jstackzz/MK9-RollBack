"""Words in a dump that point into [lo, hi): who references a heap block.
Usage: ptrsearch.py dump.bin 0xLO 0xHI [--limit N]"""
import sys

import numpy as np

sys.path.insert(0, __import__("os").path.dirname(__file__))
from memdiff import load  # noqa: E402

dump = load(sys.argv[1])
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
limit = int(sys.argv[sys.argv.index("--limit") + 1]) if "--limit" in sys.argv else 100
n = 0
for addr, d in dump:
    w = d[: len(d) // 4 * 4].view(">u4")
    for i in np.nonzero((w >= lo) & (w < hi))[0]:
        print(f"0x{addr + int(i) * 4:08X} -> 0x{int(w[i]):08X}")
        n += 1
        if n >= limit:
            sys.exit(0)
print(f"# {n} references", file=sys.stderr)
