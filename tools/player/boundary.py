#!/usr/bin/env python3
"""What the ready-made mk9.exe and a player's mk9game.dll take from each other (mk9/player/README.md).

Read from the objects of mk9.exe, just before it links:
  the game's functions mk9.exe replaces (hooks) or names the checks of (midasm hooks) -> mk9.exe exports them
  the game's functions mk9.exe calls                                                  -> mk9game.dll exports them

Writes into --out:
  host_exports.def    what mk9.exe exports
  mk9game.def + .lib  mk9game.dll's exports, as an import library for linking mk9.exe
  game_link.rsp       /EXPORT lines for linking mk9game.dll on the player's PC

A hook works because the translated code defines every game function under its own name only as a weak alias of the
original. mk9game.dll takes the hooks from mk9.exe's import library, and a weak name does not pull a library member in:
build.ninja gives the linker that library with /WHOLEARCHIVE, or every hook would be left out without a word.
"""
import argparse
import pathlib
import re
import subprocess
import sys

GAME_FUNCTION = re.compile(r"^(__imp__)?sub_[0-9A-F]{8}$")
HOOK = re.compile(r"^(sub_[0-9A-F]{8}|__imp__\w+|rexcrt_\w+)$")


def symbols(llvm: pathlib.Path, objects: list[str]) -> tuple[set[str], set[str]]:
    response = pathlib.Path(args.out) / "objects.rsp"
    response.write_text("\n".join(f'"{o}"' for o in objects), encoding="utf-8")
    text = subprocess.run([str(llvm / "llvm-nm"), "--format=posix", f"@{response}"], check=True, capture_output=True,
                          encoding="utf-8", errors="replace").stdout
    defined: set[str] = set()
    undefined: set[str] = set()
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 2 or line.endswith(":"):
            continue
        name, kind = fields[0], fields[1]
        if kind == "U":
            undefined.add(name)
        elif kind.isupper() or kind in "wWvV":
            defined.add(name)
    return defined, undefined - defined


parser = argparse.ArgumentParser()
parser.add_argument("--llvm", required=True, help="the folder of llvm-nm and llvm-lib")
parser.add_argument("--config", required=True, help="mk9_codegen.toml: the midasm hooks' names")
parser.add_argument("--out", required=True)
parser.add_argument("objects", nargs="+", help="mk9.exe's objects")
args = parser.parse_args()

out = pathlib.Path(args.out)
out.mkdir(parents=True, exist_ok=True)
llvm = pathlib.Path(args.llvm)
defined, undefined = symbols(llvm, [o for o in args.objects if o.endswith(".obj")])

checks = set(re.findall(r'^\s*name\s*=\s*"(\w+)"', pathlib.Path(args.config).read_text(encoding="utf-8"), re.M))
hooks = sorted(s for s in defined if HOOK.match(s) or any(s.startswith(f"?{c}@@") for c in checks))
wanted = sorted(s for s in undefined if GAME_FUNCTION.match(s))
found_checks = {c for c in checks if any(s.startswith(f"?{c}@@") for s in hooks)}
if found_checks != checks:
    sys.exit(f"mk9.exe does not define the checks {sorted(checks - found_checks)}")
if not hooks or not wanted:
    sys.exit(f"nothing crosses between mk9.exe and the game ({len(hooks)} hooks, {len(wanted)} calls): wrong objects?")

(out / "host_exports.def").write_text("EXPORTS\n" + "".join(f'  "{s}"\n' for s in hooks), encoding="utf-8")
(out / "mk9game.def").write_text("LIBRARY mk9game.dll\nEXPORTS\n  Mk9GameImage\n" + "".join(f"  {s}\n" for s in wanted),
                                 encoding="utf-8")
subprocess.run([str(llvm / "llvm-lib"), "/nologo", "/machine:x64", f"/def:{out / 'mk9game.def'}",
                f"/out:{out / 'mk9game.lib'}"], check=True)
lines = ["/EXPORT:Mk9GameImage\n"]
lines += [f"/EXPORT:{s}\n" for s in wanted]
(out / "game_link.rsp").write_text("".join(lines), encoding="utf-8")
print(f"mk9.exe gives the game {len(hooks)} functions and takes {len(wanted)}")
