"""Dump words of the MK9 image around an address. Usage: dumpdata.py image.bin addr before after"""
import struct
import sys

BASE = 0x82000000
b = open(sys.argv[1], "rb").read()
a = int(sys.argv[2], 16)
before = int(sys.argv[3])
after = int(sys.argv[4])
for x in range(a - before * 4, a + after * 4, 4):
    w = struct.unpack_from(">I", b, x - BASE)[0]
    mark = "<--" if x == a else ""
    kind = "code" if 0x82360000 <= w < 0x83376864 else ("rdata" if 0x82000000 <= w < 0x82360000 else ("data" if 0x83380000 <= w < 0x83560000 else ""))
    print(f"{x:08X}: {w:08X} {kind:5} {mark}")
