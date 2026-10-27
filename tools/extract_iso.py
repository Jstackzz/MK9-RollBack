"""Extract the game partition of an Xbox 360 disc image (XGD1/XGD2/XGD3, GDFX file system).

Usage: python extract_iso.py <image.iso> <out_dir> [--list]

The GDFX (XDVDFS) volume descriptor sits 32 sectors into the game partition; directory
entries form a binary tree addressed in 4-byte units inside each directory's extent.
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game partition offsets: XGD2, XGD1, XGD3, plain XISO (no video partition).
PARTITIONS = (0xFD90000, 0x18300000, 0x2080000, 0x0)
ATTR_DIRECTORY = 0x10


def find_partition(f):
    for base in PARTITIONS:
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    raise SystemExit("no GDFX volume descriptor found: not an Xbox 360 game disc image")


def read_dir(f, base, sector, size):
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    entries = []
    if size == 0:
        return entries
    stack = [0]
    seen = set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF and right == 0xFFFF:
            continue
        name = data[off + 14: off + 14 + nlen].decode("latin-1")
        entries.append((name, start, length, attr))
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)
    return entries


def walk(f, base, sector, size, prefix=""):
    for name, start, length, attr in sorted(read_dir(f, base, sector, size), key=lambda e: e[0].lower()):
        path = prefix + name
        if attr & ATTR_DIRECTORY:
            yield path, start, length, True
            yield from walk(f, base, start, length, path + "/")
        else:
            yield path, start, length, False


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    list_only = "--list" in sys.argv
    if len(args) < 1 or (len(args) < 2 and not list_only):
        raise SystemExit(__doc__)
    image = args[0]
    out = args[1] if len(args) > 1 else None
    with open(image, "rb") as f:
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + 20)
        root_sector, root_size = struct.unpack("<II", f.read(8))
        print(f"partition 0x{base:X}, root sector {root_sector}, root size {root_size}", flush=True)
        total = 0
        count = 0
        for path, start, length, is_dir in walk(f, base, root_sector, root_size):
            if list_only:
                print(f"{'<DIR>' if is_dir else length:>12} {path}")
                continue
            dest = os.path.join(out, *path.split("/"))
            if is_dir:
                os.makedirs(dest, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            if os.path.exists(dest) and os.path.getsize(dest) == length:
                total += length
                count += 1
                continue
            f.seek(base + start * SECTOR)
            remaining = length
            with open(dest + ".part", "wb") as o:
                while remaining:
                    chunk = f.read(min(remaining, 8 << 20))
                    if not chunk:
                        raise SystemExit(f"short read in {path}")
                    o.write(chunk)
                    remaining -= len(chunk)
            os.replace(dest + ".part", dest)
            total += length
            count += 1
            if count % 200 == 0:
                print(f"  {count} files, {total / 1e9:.2f} GB", flush=True)
        if not list_only:
            print(f"done: {count} files, {total / 1e9:.2f} GB", flush=True)


if __name__ == "__main__":
    main()
