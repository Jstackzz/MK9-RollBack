"""Find 32-bit big-endian words in the MK9 image. Usage: findword.py image.bin hexword [hexword...]"""
import sys

BASE = 0x82000000
b = open(sys.argv[1], "rb").read()
for w in sys.argv[2:]:
    pat = int(w, 16).to_bytes(4, "big")
    i = b.find(pat)
    hits = []
    while i >= 0 and len(hits) < 20:
        if i % 4 == 0:
            hits.append(BASE + i)
        i = b.find(pat, i + 1)
    print(w, " ".join(f"0x{h:08X}" for h in hits) or "none")
