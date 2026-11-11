#!/usr/bin/env python3
"""The server's match log from this PC: `python3 -m server.matchlog` run on the server over ssh (read only), the
matches lately and how smooth each fight was on both players' PCs (docs/PROTOCOL.md, "Match stats").

    python tools/matchlog.py                                  the last 24 hours, as text
    python tools/matchlog.py --hours 6 --player Name          one player's
    python tools/matchlog.py --match 5a9e1f5b7a69             one match, second by second
    python tools/matchlog.py --html dist/matchlog.html --open  the page, opened in the browser

The server's address is dist/defaults.ini's ([server] address, what the release ships; --host for another); ssh signs
in as ubuntu (--user) with %USERPROFILE%\\.ssh\\mk3d-server.key (--key). The address is never printed.
"""

from __future__ import annotations

import argparse
import configparser
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NAME = re.compile(r"[A-Za-z0-9_.-]{1,40}")


def server_host() -> str:
    ini = configparser.ConfigParser()
    ini.read(ROOT / "dist" / "defaults.ini", encoding="utf-8")
    address = ini.get("server", "address", fallback="").strip().strip('"')
    if not address:
        sys.exit("no [server] address in dist/defaults.ini: give --host")
    return address.rsplit(":", 1)[0] if address.count(":") == 1 else address


def ssh_exe() -> str:
    for candidate in (shutil.which("ssh"), r"C:\Windows\System32\OpenSSH\ssh.exe"):
        if candidate and Path(candidate).exists():
            return candidate
    sys.exit("ssh not found")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--hours", type=float, default=24.0)
    ap.add_argument("--player", default=None)
    ap.add_argument("--match", default=None)
    ap.add_argument("--json", action="store_true", help="the match log as JSON")
    ap.add_argument("--html", default=None, metavar="FILE", help="write the page here")
    ap.add_argument("--open", action="store_true", help="open the page once written")
    ap.add_argument("--host", default=None)
    ap.add_argument("--user", default="ubuntu")
    ap.add_argument("--key", default=str(Path.home() / ".ssh" / "mk3d-server.key"))
    args = ap.parse_args()
    for value in (args.player, args.match):
        if value is not None and not NAME.fullmatch(value):
            sys.exit(f"not a name or match id: {value!r}")
    host = args.host or server_host()
    remote = ["--hours", str(args.hours)]
    if args.player:
        remote += ["--player", args.player]
    if args.match:
        remote += ["--match", args.match]
    if args.html:
        remote.append("--html")
    elif args.json:
        remote.append("--json")
    # the database the service runs with (its --db), then the log over it, as root (the database is the service's)
    command = ("db=$(systemctl show -p ExecStart mk9-server | tr ' ' '\\n' | grep -A1 -x -- --db | tail -1) && "
               "cd /opt/mk9 && sudo -n python3 -B -m server.matchlog --db \"$db\" " + " ".join(shlex.quote(a) for a in remote))
    run = subprocess.run([ssh_exe(), "-i", args.key, "-o", "BatchMode=yes", "-o", "ConnectTimeout=15",
                          f"{args.user}@{host}", command], capture_output=True)
    err = run.stderr.decode("utf-8", "replace").replace(host, "<server>")
    if run.returncode != 0:
        sys.stderr.write(err)
        return run.returncode
    if args.html:
        out = Path(args.html)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(run.stdout)
        print(f"wrote {out} ({len(run.stdout):,} bytes)")
        if args.open:
            os.startfile(out.resolve())  # noqa: S606 (the page just written)
        return 0
    sys.stdout.write(run.stdout.decode("utf-8", "replace"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
