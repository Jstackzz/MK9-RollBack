"""A real match played again: a stand-in for the launcher's spectator feed (FeedLink) that sends a spectator's game
what a recorded match's two players sent (mk9/net/netplay.cpp, the feed's packets), from a game's net log
(MK9_NET_LOG; a player's or a spectator's, in a player's session folder as net-<match>.log): for each netplay match
in it (frame numbers start again at 0 for each), both slots' MEET, then their inputs (OWN). The last line for a frame
wins (a player's log has "r" lines for frames run again). Match 1's seed is the start line's seed=, each later match k
meets with 0x89ABCDEF ^ (k - 1); --version is the players' game release (its stamp is checked).
Start a game linked (MK9_LINK=1, see link-local.ps1) and, at its hold point, give it the spectator's start line from
that session's game.log with feed=127.0.0.1:<port>: it plays the match frame for frame as the players did (their
checks are in the log to compare), with MK9_DIAG_DUMP / MK9_SNAP_FRAMES / MK9_TRACE_SCRIPT as wanted.
--gap 0 sends everything at once, as a spectator of a match under way gets it; a gap of a few seconds between
matches is what a spectator watching from the start gets.
From 0.5.5 on a fight starts with slot 0's frame counter: give each fight's with --counters (slot 0's game.log, the
"frame counter" of the fight's "agreed seed" line), or the two fighters' scripts may run in the other order.
Usage: feedreplay.py --seed 3997265532 [--port 7302] [--version 0.5.2] [--delay 2] [--gap 0] [--counters D5,1A0] <net log>"""
import argparse
import socket
import struct
import time

MAGIC = 0x4E394B4D
MEET, OWN, END = 6, 7, 9


def fnv(data: bytes) -> int:
    h = 2166136261
    for c in data:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def wire(field: str) -> bytes:
    buttons, triggers, sticks = field.split(":")
    b = int(buttons, 16)
    lt, rt = int(triggers[0:2], 16), int(triggers[2:4], 16)
    s = [int(sticks[i:i + 2], 16) for i in range(0, 8, 2)]
    return struct.pack("<HBB4B", b, lt, rt, *s)


def matches(path: str):
    out, cur, last = [], {}, -1
    for line in open(path, encoding="ascii", errors="replace"):
        parts = line.split()
        if len(parts) < 3:
            continue
        tok = parts[0]
        again = tok.startswith("r")
        frame = int(tok[1:] if again else tok)
        if not again and frame == 0 and last >= 0:
            out.append(cur)
            cur = {}
        cur[frame] = parts[1:3]
        if not again:
            last = frame
    if cur:
        out.append(cur)
    return out


def packet(ptype: int, slot: int, match: int, body: bytes) -> bytes:
    p = struct.pack("<IBBH", MAGIC, ptype, slot, match) + body
    return struct.pack("<H", len(p)) + p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=7302)
    ap.add_argument("--seed", type=lambda v: int(v, 0), required=True)
    ap.add_argument("--version", default="0.5.2")
    ap.add_argument("--delay", type=int, default=2)
    ap.add_argument("--end", action="store_true", help="END for both slots after the last match")
    ap.add_argument("--gap", type=float, default=0.0)
    ap.add_argument("--counters", default="",
                    help="0.5.5 on: each fight's frame counter as slot 0's game.log says it was agreed (\"frame counter "
                         "<hex>\" of its \"agreed seed\" line), comma-separated, matches 2, 3, ...")
    ap.add_argument("log")
    a = ap.parse_args()
    stamp = fnv(a.version.encode())
    counters = [int(c, 16) for c in a.counters.split(",") if c]
    parts = []
    ms = matches(a.log)
    for k, frames in enumerate(ms, start=1):
        feed = bytearray()
        seed = a.seed if k == 1 else (0x89ABCDEF ^ (k - 1))
        n = max(frames) + 1
        # the words of a READY: the release's stamp, the round timer (unused), and a fight's frame counter (a fight's
        # barrier takes slot 0's: mk9/src/mk9/game.cpp, ReadyWords)
        words = [stamp, 0] + ([counters[k - 2]] if 2 <= k < len(counters) + 2 else [])
        print(f"match {k}: frames 0..{n - 1}, seed {seed:08X}, words {' '.join(f'{w:X}' for w in words)}", flush=True)
        for slot in (0, 1):
            body = struct.pack("<IB", seed, len(words)) + b"".join(struct.pack("<I", w) for w in words)
            feed += packet(MEET, slot, k, body + struct.pack("<B", a.delay))
        for slot in (0, 1):
            for first in range(0, n, 128):
                chunk = [wire(frames[f][slot]) if f in frames else bytes(8) for f in range(first, min(n, first + 128))]
                feed += packet(OWN, slot, k, struct.pack("<IB", first, len(chunk)) + b"".join(chunk))
        if a.end and k == len(ms):
            for slot in (0, 1):
                feed += packet(END, slot, k, struct.pack("<I", n))
        parts.append(bytes(feed))
    print(f"{sum(map(len, parts))} bytes of feed, {len(ms)} matches", flush=True)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", a.port))
    srv.listen(4)
    while True:
        c, peer = srv.accept()
        print(f"spectator {peer} connected: sending the feed", flush=True)
        try:
            for i, part in enumerate(parts):
                if i:
                    time.sleep(a.gap)
                c.sendall(part)
                print(f"sent match {i + 1}", flush=True)
            while c.recv(4096):
                pass
        except OSError as e:
            print(f"spectator {peer}: {e}", flush=True)
        finally:
            c.close()
        print(f"spectator {peer} gone", flush=True)


if __name__ == "__main__":
    main()
