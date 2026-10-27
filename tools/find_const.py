"""Find code that builds 32-bit constants with lis + ori/addi (e.g. a random generator's multiplier).
Usage: find_const.py image.bin 0x41C64E6D [0x343FD ...]"""
import struct
import sys

BASE = 0x82000000
TEXT_LO, TEXT_HI = 0x82360000, 0x83376864
b = open(sys.argv[1], "rb").read()
targets = {int(x, 0) & 0xFFFFFFFF for x in sys.argv[2:]}


def word(a):
    return struct.unpack_from(">I", b, a - BASE)[0]


for a in range(TEXT_LO, TEXT_HI - 4, 4):
    w = word(a)
    if (w >> 26) != 15 or ((w >> 16) & 31) != 0:
        continue
    rd = (w >> 21) & 31
    hi = (w & 0xFFFF) << 16
    for k in range(1, 8):
        x = word(a + 4 * k)
        op = x >> 26
        if op == 24 and ((x >> 21) & 31) == rd:  # ori rA,rS,UIMM (rS = lis reg)
            v = (hi | (x & 0xFFFF)) & 0xFFFFFFFF
        elif op == 14 and ((x >> 16) & 31) == rd:  # addi rD,rA,SIMM
            lo = x & 0xFFFF
            v = (hi + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
        else:
            continue
        if v in targets:
            print(f"0x{a:08X}: builds 0x{v:08X}")
        break
