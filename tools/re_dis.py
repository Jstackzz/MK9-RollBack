"""Print the disassembly comments of generated functions by start address.
Usage: dis.py <generated_dir> <addr> [addr...] [--max N]"""
import glob
import os
import re
import sys

gen = sys.argv[1]
args = sys.argv[2:]
maxl = 400
addrs = []
i = 0
while i < len(args):
    if args[i] == "--max":
        maxl = int(args[i + 1]); i += 2; continue
    addrs.append(args[i].upper().replace("0X", "")); i += 1

index = {}
for f in sorted(glob.glob(os.path.join(gen, "mk9_recomp.*.cpp"))):
    text = open(f).read()
    for m in re.finditer(r"^DEFINE_REX_FUNC\(([A-Za-z0-9_]+)\)", text, re.M):
        index[m.group(1)] = (f, m.start())

for a in addrs:
    key = next((k for k in index if k.lower().endswith(a.lower())), None)
    if not key:
        print(f"=== {a}: not found")
        continue
    f, pos = index[key]
    text = open(f).read()
    end = text.find("\nDEFINE_REX_FUNC(", pos + 10)
    body = text[pos:end if end > 0 else len(text)]
    print(f"=== {key}")
    n = 0
    for l in body.splitlines()[1:]:
        s = l.strip()
        if s.startswith("//") or s.startswith("loc_") or "REX_FATAL" in s or s.startswith("sub_") or "rexcrt_" in s or "__imp__" in s:
            print("   ", s)
            n += 1
            if n >= maxl:
                print("    ...")
                break
