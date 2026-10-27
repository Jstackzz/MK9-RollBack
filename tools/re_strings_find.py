"""Find ASCII and UTF-16BE/LE strings matching patterns in the MK9 image; print VA."""
import re
import sys

BASE = 0x82000000
data = open(sys.argv[1], "rb").read()
pats = [p.lower() for p in sys.argv[2:]]

ascii_re = re.compile(rb"[\x20-\x7e]{5,}")
for m in ascii_re.finditer(data):
    s = m.group().decode("ascii")
    if any(p in s.lower() for p in pats):
        print(f"A 0x{BASE + m.start():08X} {s[:160]}")

# UTF-16 big endian (Xbox 360 TCHAR is wchar_t, stored big endian)
u16be = re.compile(rb"(?:\x00[\x20-\x7e]){5,}")
for m in u16be.finditer(data):
    s = m.group().decode("utf-16-be")
    if any(p in s.lower() for p in pats):
        print(f"W 0x{BASE + m.start():08X} {s[:160]}")
