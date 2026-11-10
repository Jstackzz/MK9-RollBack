"""Two games' script traces (MK9_TRACE_SCRIPT), the way tools/scriptcmp.py compares them but with object ids blanked and which thread made an
effect ignored: for a pair whose object ids differ on purpose (MK9_TEST_BURN_IDS), where scriptcmp flags every frame that makes an object.
Prints, per match after the menus', how many frames differ and the first few, with what only one game did.
Usage: python tools/noidcmp.py <run dir holding a/script.txt and b/script.txt>"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(HERE, "scriptcmp.py"), encoding="utf-8").read()
ns = {}
exec(compile(src[: src.index("def net_inputs")], "scriptcmp.py", "exec"), ns)
matches = ns["matches"]


def blank(event):
    event = re.sub(r"\bid [0-9A-F]+", "id _", event)
    return re.sub(r"thread [mo]", "thread ?", event)


run = sys.argv[1]
a = matches(os.path.join(run, "a", "script.txt"))
b = matches(os.path.join(run, "b", "script.txt"))
for m, (ma, mb) in enumerate(zip(a, b), 1):
    if m == 1:
        continue  # the menus and loading: each game's own loading time shows there
    frames = sorted(set(ma) & set(mb))
    if not frames:
        continue
    last = min(max(ma), max(mb))
    bad = []
    for f in frames:
        if f >= last - 2:  # a game stopped mid-frame
            continue
        if sorted(map(blank, ma.get(f, []))) != sorted(map(blank, mb.get(f, []))):
            bad.append(f)
    print(f"match {m}: {len(frames)} frames, {len(bad)} differ with ids blanked" + (f": {bad[:20]}" if bad else ""))
    for f in bad[:6]:
        ea, eb = list(map(blank, ma.get(f, []))), list(map(blank, mb.get(f, [])))
        for x in list(ea):
            if x in eb:
                ea.remove(x)
                eb.remove(x)
        print(f"  frame {f}: A only {ea[:4]}  B only {eb[:4]}")
