"""Print the generated C++ around guest addresses: finds the function containing each address
(via the PPCFuncMappings in mk9_init.cpp) and shows the disassembly comments near it.
Usage: find_ctx.py <generated_dir> <addr> [addr...] [--before N] [--after N]"""
import bisect
import glob
import os
import re
import sys

gen = sys.argv[1]
args = sys.argv[2:]
before = 12
after = 12
addrs = []
i = 0
while i < len(args):
    if args[i] == "--before":
        before = int(args[i + 1]); i += 2; continue
    if args[i] == "--after":
        after = int(args[i + 1]); i += 2; continue
    addrs.append(int(args[i], 16)); i += 1

starts = sorted(int(m, 16) for m in re.findall(r"\{ 0x([0-9A-F]{8}), sub_", open(os.path.join(gen, "mk9_init.cpp")).read()))
files = sorted(glob.glob(os.path.join(gen, "mk9_recomp.*.cpp")))
func_file = {}
for f in files:
    for m in re.finditer(r"DEFINE_REX_FUNC\((?:sub_|__)?([^)]*)\)", open(f).read()):
        func_file[m.group(1)] = f

for a in addrs:
    k = bisect.bisect_right(starts, a) - 1
    fstart = starts[k] if k >= 0 else None
    nxt = starts[k + 1] if k + 1 < len(starts) else None
    print(f"=== 0x{a:08X}: in function 0x{fstart:08X} (next function 0x{nxt:08X})" if fstart else f"=== 0x{a:08X}: before any function")
    # find the function text
    name = f"{fstart:08X}"
    path = func_file.get(name)
    if not path:
        print("  function body not found in recomp files")
        continue
    lines = open(path).read().splitlines()
    begin = next(i for i, l in enumerate(lines) if f"DEFINE_REX_FUNC(sub_{name})" in l)
    # walk instructions: every '// ' comment is one instruction; label lines 'loc_' carry addresses
    cur = fstart
    target_line = None
    for j in range(begin + 1, len(lines)):
        l = lines[j]
        if l.startswith("DEFINE_REX_FUNC("):
            break
        m = re.match(r"\s*loc_([0-9A-F]{8}):", l)
        if m:
            cur = int(m.group(1), 16)
            if cur == a:
                target_line = j
            continue
        if re.match(r"\s*// ", l) and not l.strip().startswith("// ---"):
            if cur == a and target_line is None:
                target_line = j
            cur += 4
    if target_line is None:
        print("  address not inside the emitted body")
        continue
    for l in lines[max(begin, target_line - before):target_line + after]:
        if re.match(r"\s*(//|loc_)", l):
            print("   ", l.strip())
