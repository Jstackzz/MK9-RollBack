#!/usr/bin/env python3
"""Compares the machine code of two builds of the translated game, function by function (mk9/player/README.md).

Usage: compare_objs.py <folder of NAME.obj> <folder of NAME.cpp.obj>
The first is a compile against mk9/player/include (with the flags in build.ninja), the second a normal build's
(mk9/out/build/win-amd64-release/CMakeFiles/mk9_recomp.dir/generated/default). For a clean comparison compile the first
with the second's exception flags (-fcxx-exceptions -fexceptions -fasync-exceptions) and with the stand-in logging
macros made big enough that ppc_trap is not inlined. What is left should be guest locks, memory barriers and ppc_trap.
"""
import collections
import difflib
import os
import pathlib
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

OBJDUMP = pathlib.Path(__file__).resolve().parents[2] / "external" / "llvm" / "bin" / "llvm-objdump.exe"
ADDRESS = re.compile(r"^\s*[0-9a-f]+:\s*")
TARGET = re.compile(r"\b0x[0-9a-f]+ <([^>+]+)(\+0x[0-9a-f]+)?>")
RIP_COMMENT = re.compile(r"\s*# 0x[0-9a-f]+.*$")


def functions(path: str) -> dict[str, list[str]]:
    text = subprocess.run([str(OBJDUMP), "-d", "--no-show-raw-insn", path], capture_output=True, text=True).stdout
    result: dict[str, list[str]] = {}
    current = None
    for line in text.splitlines():
        start = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
        if start:
            current = start.group(1)
            result[current] = []
        elif current and line.strip() and not line.startswith("Disassembly"):
            line = RIP_COMMENT.sub("", ADDRESS.sub("", line))
            line = TARGET.sub("<>", line)
            result[current].append(re.sub(r"\s+", " ", line).strip())
    return result


def compare(name: str):
    a = functions(os.path.join(sys.argv[1], name + ".obj"))
    b = functions(os.path.join(sys.argv[2], name + ".cpp.obj"))
    return len(a), {(name, f): (a[f], b[f]) for f in a if f in b and a[f] != b[f]}


names = [f[:-4] for f in os.listdir(sys.argv[1]) if f.endswith(".obj")]
total = 0
differing = {}
with ThreadPoolExecutor(14) as pool:
    for count, found in pool.map(compare, names):
        total += count
        differing.update(found)
print(f"{total} functions compared, {len(differing)} differ")
shapes = collections.Counter()
for new, old in differing.values():
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, old, new, autojunk=False).get_opcodes():
        if tag != "equal":
            shapes[" | ".join(old[i1:i2])[:160] + "   ==>   " + " | ".join(new[j1:j2])[:160]] += 1
for shape, count in shapes.most_common(40):
    print(count, shape)
