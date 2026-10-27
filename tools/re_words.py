"""Dump raw PPC words from the MK9 image with a rough decode of control-flow instructions,
and report which pdata function (if any) covers each address.
Usage: words.py image.bin addr count"""
import bisect
import struct
import sys

BASE = 0x82000000
b = open(sys.argv[1], "rb").read()
addr = int(sys.argv[2], 16)
count = int(sys.argv[3])

p = 0x2B3600
fn = []
for i in range(0xA2500 // 8):
    beg, w = struct.unpack_from(">II", b, p + i * 8)
    if beg:
        fn.append((beg, beg + ((w >> 8) & 0x3FFFFF) * 4))
fn.sort()
starts = [f[0] for f in fn]


def pdata_of(a):
    k = bisect.bisect_right(starts, a) - 1
    if k >= 0 and fn[k][0] <= a < fn[k][1]:
        return f"pdata {fn[k][0]:08X}-{fn[k][1]:08X}"
    return "no pdata"


def dec(a, w):
    op = w >> 26
    if w == 0x4E800020:
        return "blr"
    if w == 0x4E800420:
        return "bctr"
    if w == 0x4E800421:
        return "bctrl"
    if w == 0x7D8802A6:
        return "mflr r12"
    if op == 18:
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        t = (a + li) if not (w & 2) else li
        return f"b{'l' if w & 1 else ''} 0x{t & 0xFFFFFFFF:08X}"
    if op == 16:
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        return f"bc{'l' if w & 1 else ''} BO={(w >> 21) & 31} BI={(w >> 16) & 31} 0x{(a + bd) & 0xFFFFFFFF:08X}"
    if op == 19 and ((w >> 1) & 0x3FF) == 16:
        return f"bclr BO={(w >> 21) & 31} BI={(w >> 16) & 31}"
    if op == 0 and w == 0:
        return "(zero)"
    return ""


for i in range(count):
    a = addr + i * 4
    w = struct.unpack_from(">I", b, a - BASE)[0]
    print(f"{a:08X}: {w:08X}  {dec(a, w):34} {pdata_of(a)}")
