"""Which recompiled function contains each guest address. Usage: funcof.py <generated_dir> addr..."""
import bisect
import os
import re
import sys

gen = sys.argv[1]
text = open(os.path.join(gen, "mk9_init.cpp")).read()
pairs = sorted((int(a, 16), n) for a, n in re.findall(r"\{ 0x([0-9A-F]{8}), (\w+) \}", text))
starts = [p[0] for p in pairs]
for arg in sys.argv[2:]:
    a = int(arg, 16)
    k = bisect.bisect_right(starts, a) - 1
    print(f"0x{a:08X} in {pairs[k][1]} (+0x{a - pairs[k][0]:X})" if k >= 0 else f"0x{a:08X}: none")
