"""Search memory dumps of the game (mk9 MK9_DIAG_DUMP) for variables.

  memdiff.py seq <dump1> <dump2> ... --values 0,1,0 [--width 4|2|1] [--signed]
      addresses whose big-endian value is values[i] in dump i (in the order given)
  memdiff.py changed <dumpA> <dumpB> [--width 4] [--limit 200]
      addresses whose value differs (counts per region; first hits)
  memdiff.py same-diff <a1> <a2> <b1> <b2> ...
      addresses equal within the a-dumps, equal within the b-dumps, and different between them
  memdiff.py read <dump> <hexaddr> [count]
      words at an address
  memdiff.py dec <dump1> <dump2> <dump3> ... [--float] [--start V]
      addresses whose value strictly decreases from dump to dump (int32, or float32 with --float),
      starting at V in the first dump when given (e.g. a full health bar)

A dump is records of u32 address, u32 size (native little endian) followed by the bytes.
"""
import struct
import sys

import numpy as np


def load(path):
    regions = []
    with open(path, "rb") as f:
        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                break
            addr, size = struct.unpack("<II", hdr)
            data = f.read(size)
            regions.append((addr, np.frombuffer(data, dtype=np.uint8)))
    return regions


def as_words(buf, width, signed):
    n = len(buf) // width * width
    dt = {1: "u1", 2: ">u2", 4: ">u4"}[width]
    if signed:
        dt = dt.replace("u", "i")
    return buf[:n].view(dt)


def opt(args, name, default):
    if name in args:
        i = args.index(name)
        v = args[i + 1]
        del args[i:i + 2]
        return v
    return default


def flag(args, name):
    if name in args:
        args.remove(name)
        return True
    return False


def common_regions(dumps):
    base = {a: d for a, d in dumps[0]}
    for other in dumps[1:]:
        o = {a: d for a, d in other}
        base = {a: d for a, d in base.items() if a in o and len(o[a]) == len(d)}
    return sorted(base)


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    cmd = args.pop(0)
    width = int(opt(args, "--width", "4"))
    signed = flag(args, "--signed")
    limit = int(opt(args, "--limit", "200"))
    if cmd == "seq":
        values = [int(v, 0) for v in opt(args, "--values", "").split(",")]
        dumps = [load(p) for p in args]
        if len(values) != len(dumps):
            raise SystemExit("one value per dump")
        maps = [{a: d for a, d in dm} for dm in dumps]
        hits = 0
        for addr in common_regions(dumps):
            mask = None
            for m, v in zip(maps, values):
                w = as_words(m[addr], width, signed)
                cur = w == v
                mask = cur if mask is None else (mask & cur)
            for i in np.nonzero(mask)[0][: max(0, limit - hits)]:
                print(f"0x{addr + int(i) * width:08X}")
                hits += 1
            if hits >= limit:
                break
        print(f"# {hits} hits", file=sys.stderr)
    elif cmd == "changed":
        a, b = load(args[0]), load(args[1])
        mb = {x: d for x, d in b}
        total = 0
        for addr, da in a:
            db = mb.get(addr)
            if db is None or len(db) != len(da):
                continue
            wa, wb = as_words(da, width, False), as_words(db, width, False)
            idx = np.nonzero(wa != wb)[0]
            if len(idx):
                print(f"region 0x{addr:08X}+0x{len(da):X}: {len(idx)} changed")
                for i in idx[:8]:
                    print(f"   0x{addr + int(i) * width:08X}: {int(wa[i]):08X} -> {int(wb[i]):08X}")
                total += len(idx)
        print(f"# {total} words changed")
    elif cmd == "same-diff":
        half = len(args) // 2
        ga, gb = [load(p) for p in args[:half]], [load(p) for p in args[half:]]
        allm = [{x: d for x, d in dm} for dm in ga + gb]
        hits = 0
        for addr in common_regions(ga + gb):
            ws = [as_words(m[addr], width, False) for m in allm]
            ok = np.ones(len(ws[0]), dtype=bool)
            for w in ws[1:half]:
                ok &= w == ws[0]
            for w in ws[half + 1:]:
                ok &= w == ws[half]
            ok &= ws[0] != ws[half]
            for i in np.nonzero(ok)[0][: max(0, limit - hits)]:
                print(f"0x{addr + int(i) * width:08X}: a {int(ws[0][i]):X} b {int(ws[half][i]):X}")
                hits += 1
            if hits >= limit:
                break
        print(f"# {hits} hits", file=sys.stderr)
    elif cmd == "dec":
        use_float = flag(args, "--float")
        start = opt(args, "--start", None)
        dumps = [load(p) for p in args]
        maps = [{a: d for a, d in dm} for dm in dumps]
        hits = 0
        for addr in common_regions(dumps):
            ws = []
            for m in maps:
                buf = m[addr][: len(m[addr]) // 4 * 4]
                ws.append(buf.view(">f4" if use_float else ">i4"))
            ok = np.ones(len(ws[0]), dtype=bool)
            for prev, cur in zip(ws, ws[1:]):
                ok &= cur < prev
            if use_float:
                ok &= np.isfinite(ws[0]) & (np.abs(ws[0]) < 1e6)
            if start is not None:
                ok &= ws[0] == (float(start) if use_float else int(start, 0))
            for i in np.nonzero(ok)[0][: max(0, limit - hits)]:
                vals = " ".join(f"{float(w[i]):g}" if use_float else str(int(w[i])) for w in ws)
                print(f"0x{addr + int(i) * 4:08X}: {vals}")
                hits += 1
            if hits >= limit:
                break
        print(f"# {hits} hits", file=sys.stderr)
    elif cmd == "read":
        dump = load(args[0])
        target = int(args[1], 16)
        count = int(args[2]) if len(args) > 2 else 16
        for addr, d in dump:
            if addr <= target < addr + len(d):
                off = target - addr
                for k in range(count):
                    o = off + k * 4
                    print(f"0x{target + k * 4:08X}: {struct.unpack_from('>I', d.tobytes(), o)[0]:08X}")
                break
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
