"""Compare two games' netplay logs (MK9_NET_LOG) frame by frame.

Each line: <frame> <slot inputs...> [check <hash> timer <t> [round <n> won <a>-<b>] rand <r> [draws <d> hp <h1> <h2>
meter <m1> <m2>]] (round and won since 0.5.4's game, the last ones since 0.5.6's); "r<frame>" when a rollback
ran the frame again (that line then stands for the frame). A match's frames start at 0 again, so
the logs are split into matches where the frame number goes back.
Usage: netcmp.py a/net.log b/net.log
"""
import sys


def matches(path):
    out, cur, last = [], {}, None
    for line in open(path, encoding="ascii", errors="replace"):
        parts = line.split()
        if not parts:
            continue
        again = parts[0].startswith("r")
        f = int(parts[0].lstrip("r"))
        parts[0] = str(f)
        if not again:
            if last is not None and f <= last:
                out.append([cur[k] for k in sorted(cur)])
                cur = {}
            last = f
        cur[f] = parts
    if cur:
        out.append([cur[k] for k in sorted(cur)])
    return out


def field(parts, name):
    if name in parts:
        i = parts.index(name)
        return parts[i + 1] if i + 1 < len(parts) else None
    return None


a, b = matches(sys.argv[1]), matches(sys.argv[2])
print(f"{len(a)} matches in A, {len(b)} in B")
for m, (ma, mb) in enumerate(zip(a, b), 1):
    n = min(len(ma), len(mb))
    in_diff = check_diff = timer_diff = rand_diff = draws_diff = hp_diff = 0
    first = {}
    for i in range(n):
        pa, pb = ma[i], mb[i]
        ia = [x for x in pa[1:] if ":" in x]
        ib = [x for x in pb[1:] if ":" in x]
        if ia != ib:
            in_diff += 1
            first.setdefault("inputs", pa[0])
        if "check" in pa and "check" in pb:
            if field(pa, "check") != field(pb, "check"):
                check_diff += 1
                first.setdefault("check", pa[0])
            if field(pa, "timer") != field(pb, "timer"):
                timer_diff += 1
                first.setdefault("timer", pa[0])
            if (field(pa, "round"), field(pa, "won")) != (field(pb, "round"), field(pb, "won")):
                first.setdefault("round", pa[0])
            if field(pa, "rand") != field(pb, "rand"):
                rand_diff += 1
                first.setdefault("rand", pa[0])
            if field(pa, "draws") != field(pb, "draws"):
                draws_diff += 1
                first.setdefault("draws", pa[0])
            if "hp" in pa and "hp" in pb:
                ha, hb = pa.index("hp"), pb.index("hp")
                if pa[ha + 1 : ha + 6] != pb[hb + 1 : hb + 6]:  # both fighters' health and meters
                    hp_diff += 1
                    first.setdefault("hp", pa[0])
    print(f"match {m}: {len(ma)} / {len(mb)} frames; inputs differ {in_diff}, checks {check_diff} "
          f"(timer {timer_diff}, rand {rand_diff}, draws {draws_diff}, hp and meters {hp_diff}); first: {first}")
