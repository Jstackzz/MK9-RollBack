#!/usr/bin/env python3
"""Development stand-in for mk9.exe: speaks the launcher <-> game contract of the MK9 port (docs/LAUNCHER.md, "The
game contract"; mk9/src/mk9/link.cpp) without any game, so the launcher can be tested end to end.

Point the game's "Executable" in the launcher's Settings at this file (the launcher runs .py files with Python). Like
the game it reads MK9_LINK, MK9_CMD, MK9_STATUS (and logs MK9_INSTANCE, MK9_SESSION_DIR, MK9_NET_LOG, MK9_CONTROLS)
and takes the game's command line (--game_data_root=, --user_data_root=, --log_file=, --cache_root=,
--fullscreen / --no-fullscreen).

    booting -> ready                 (the hold point: the main menu)
    start mode=versus|tag|tyl slot=N players=2 local_port=P peers=<slot>@ip:port[,...] delay=D netcode=lockstep|rollback
          seed=S match=<id> [p1=..] [p2=..] [feed=127.0.0.1:port]
                                     binds local_port, exchanges heartbeats with the peers, writes in_match; after
                                     MK9_FAKE_SECONDS (default 6) writes a result derived from the seed (so both
                                     sides agree) and waits at its "CONTINUE?" menu; disconnected if the peer goes
                                     quiet, "error no connection" if it never answers. With feed=, it writes MK9N feed
                                     packets (MEET, OWN, END) there, after their u16 length, as a player's game does.
    start ... watch=1 feed=127.0.0.1:port   a spectator: reads the feed, in_match, then the seed's result
    start mode=practice              offline: in_match match=practice until park
    rematch [seed=N] [match=<id>] [select=1]   from CONTINUE?: the next match with the same peer
    park                             from CONTINUE?: back to ready after MK9_FAKE_PARK seconds (default 1); from the
                                     middle of a match it is lost, as the game is ("booting" until it is started again)
    quit                             exit

Without MK9_LINK=1 (single player) it just runs until it is closed. Knobs: MK9_FAKE_BOOT (seconds before "ready",
default 1.5), MK9_FAKE_RESULT=desync (report "desync frame=600", its log then has a match's "mk9 net:" lines as the
game writes them, for the launcher's desync report) or none (never report a result, a match that does not get to its
end), MK9_FAKE_EXIT=<code> (the game ends by itself with that exit code halfway through a match: a crash or a closed
window, for the launcher's "left" report).
"""

from __future__ import annotations

import json
import os
import socket
import struct
import sys
import time

FEED_MAGIC = 0x4E394B4D  # "MK9N"
FEED_MEET, FEED_OWN, FEED_CHECK, FEED_END = 6, 7, 8, 9


def simulated_result(seed: int, rounds: int = 3) -> tuple[int, int, int]:
    """(winner, p1_rounds, p2_rounds) from the seed, the same rule as server/client.py."""
    need = rounds // 2 + 1
    winner = 1 if seed % 2 == 0 else 2
    loser_rounds = (seed >> 1) % need
    return (winner, need, loser_rounds) if winner == 1 else (winner, loser_rounds, need)


LOG_FILE = None


def log(text: str) -> None:
    line = f"[fake_game {os.environ.get('MK9_INSTANCE', '1')}] {text}"
    print(line, flush=True)
    if LOG_FILE:
        try:
            with open(LOG_FILE, "a", encoding="utf-8") as fh:
                fh.write(line + "\n")
        except OSError:
            pass


def write_atomic(path: str, text: str) -> None:
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="ascii", newline="\n") as fh:
        fh.write(text + "\n")
    for _ in range(200):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            time.sleep(0.005)
    log(f"could not replace {path}")


def take_command(path: str) -> str | None:
    taken = path + ".taken"
    try:
        os.replace(path, taken)
    except (FileNotFoundError, PermissionError):
        return None
    with open(taken, encoding="ascii", errors="replace") as fh:
        return fh.readline().strip()


def parse(line: str) -> tuple[str, dict[str, str]]:
    verb, *rest = line.split()
    keys = {}
    for token in rest:
        if "=" in token:
            k, _, v = token.partition("=")
            keys[k] = v
    return verb, keys


def endpoint(text: str) -> tuple[str, int]:
    host, _, port = text.rpartition(":")
    return host, int(port)


def feed_packet(kind: int, slot: int, match: int, payload: bytes = b"") -> bytes:
    body = struct.pack("<IBBH", FEED_MAGIC, kind, slot, match & 0xFFFF) + payload
    return struct.pack("<H", len(body)) + body


class FakeGame:
    def __init__(self, status_path: str, cmd_path: str) -> None:
        self.status_path = status_path
        self.cmd_path = cmd_path
        self.match_seconds = float(os.environ.get("MK9_FAKE_SECONDS", "6"))
        self.result_mode = os.environ.get("MK9_FAKE_RESULT", "")
        self.exit_code = int(os.environ["MK9_FAKE_EXIT"]) if os.environ.get("MK9_FAKE_EXIT") else None
        self.sock: socket.socket | None = None
        self.feed: socket.socket | None = None
        self.state = "booting"
        self.start: dict[str, str] = {}
        self.match = ""
        self.peers: list[tuple[str, int]] = []
        self.started_at = 0.0
        self.connected_at = 0.0
        self.last_peer = 0.0
        self.last_send = 0.0
        self.sent = 0
        self.barrier = 0
        self.ready_at = 0.0  # a park on its way back to the main menu

    def status(self, line: str) -> None:
        log(f"status: {line}")
        write_atomic(self.status_path, line)

    def close_socket(self) -> None:
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def close_feed(self) -> None:
        if self.feed is not None:
            try:
                self.feed.close()
            except OSError:
                pass
            self.feed = None

    def slot(self) -> int:
        if "slot" in self.start:
            return int(self.start["slot"])
        return 1 if self.start.get("side") == "2" else 0

    def open_feed(self, keys: dict[str, str]) -> None:
        self.close_feed()
        if not keys.get("feed"):
            return
        try:
            self.feed = socket.create_connection(endpoint(keys["feed"]), timeout=2)
            self.feed.setblocking(False)
            log(f"feed connected to {keys['feed']}")
        except OSError as exc:
            log(f"feed {keys['feed']}: {exc}")
            self.feed = None

    def feed_write(self, data: bytes) -> None:
        if self.feed is None or self.start.get("watch") == "1":
            return
        try:
            self.feed.sendall(data)
        except OSError:
            self.close_feed()

    def begin(self, keys: dict[str, str]) -> None:
        self.close_socket()
        self.start = keys
        self.match = keys.get("match", "none")
        self.started_at = time.monotonic()
        self.connected_at = 0.0
        self.last_peer = 0.0
        self.barrier += 1
        if keys.get("mode") == "practice":
            self.state = "practice"
            self.status("in_match match=practice")
            return
        if keys.get("mode") not in ("versus", "tag", "tyl"):
            self.state = "ready"
            self.status(f"error bad command mode {keys.get('mode')}")
            return
        self.open_feed(keys)
        if keys.get("watch") == "1":
            if self.feed is None:
                self.state = "ready"
                self.status("error watch needs feed")
                return
            self.state = "watching"
            self.connected_at = time.monotonic()
            self.status(f"in_match match={self.match}")
            log(f"watching match {self.match} from the feed, seed {keys.get('seed')}")
            return
        self.peers = []
        for item in keys.get("peers", "").split(","):
            if "@" in item:
                self.peers.append(endpoint(item.partition("@")[2]))
        if not self.peers and keys.get("remote"):
            self.peers.append(endpoint(keys["remote"]))
        port = int(keys.get("local_port", "7001"))
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        loopback = all(host.startswith("127.") for host, _ in self.peers)
        # a loopback peer needs no public socket (and then Windows asks nothing about the firewall)
        self.sock.bind(("127.0.0.1" if loopback else "0.0.0.0", port))
        self.sock.setblocking(False)
        self.state = "connecting"
        self.feed_write(feed_packet(FEED_MEET, self.slot(), self.barrier, struct.pack("<IB", int(keys.get("seed", "0")) & 0xFFFFFFFF, 0)))
        log(f"{keys.get('mode')} slot {self.slot()} of {keys.get('players', '2')} on udp {port} -> {self.peers}, "
            f"{keys.get('netcode', 'lockstep')}, delay {keys.get('delay')}, seed {keys.get('seed')}, "
            f"p1={keys.get('p1', '-')} p2={keys.get('p2', '-')}")

    def finish(self) -> None:
        """The match's end: its result (or none), then the CONTINUE? menu."""
        self.state = "continue"
        self.feed_write(feed_packet(FEED_END, self.slot(), self.barrier, struct.pack("<I", 360)))
        if self.result_mode == "desync":
            slot = self.slot()
            for line in (f"mk9 link: start {self.start.get('mode', 'versus')} slot {slot} of 2, match {self.match}, "
                         f"{self.start.get('netcode', 'lockstep')}, 1 peers",
                         f"mk9 net: slot {slot} of 2, port {self.start.get('local_port', '0')}, 1 peers, "
                         f"{self.start.get('netcode', 'lockstep')} delay {self.start.get('delay', '2')}",
                         f"mk9 net: a packet from 203.0.113.9:7001, not this match's peer: ignored (1 so far)",
                         f"mk9 net: match {self.match} in sync, seed {int(self.start.get('seed', '0')) & 0xFFFFFFFF:08X}, "
                         f"input delay {self.start.get('delay', '2')}",
                         f"mk9 net: DESYNC at frame 600 with slot {1 - slot}: 1234ABCD here, 5678EF00 there"):
                log(line)
            net_log = os.environ.get("MK9_NET_LOG", "").replace("{match}", self.match).replace("{slot}", str(slot))
            if net_log:
                with open(net_log, "w", encoding="ascii") as fh:
                    for f in range(590, 601):
                        fh.write(f"{f} 0000:0000:00000000 0000:0000:00000000 check {f * 2654435761 & 0xFFFFFFFF:08X} "
                                 f"timer {f} rand {f * 40503 & 0xFFFFFFFF:08X}\n")
            self.status(f"desync frame=600 match={self.match}")
        elif self.result_mode == "none":
            self.state = "playing"  # never gets to its end
            self.connected_at = float("inf")
        else:
            seed = int(self.start.get("seed", "0"))
            winner, r1, r2 = simulated_result(seed, int(self.start.get("rounds", "3")))
            self.status(f"result winner={winner} p1_rounds={r1} p2_rounds={r2} match={self.match}")

    def handle(self, line: str) -> bool:
        log(f"cmd: {line}")
        verb, keys = parse(line)
        if verb == "quit":
            self.status("quit")
            self.close_socket()
            self.close_feed()
            return False
        if verb == "park":
            self.close_feed()
            if self.state in ("continue", "ready", "practice"):
                self.close_socket()
                self.state = "parking"
                self.ready_at = time.monotonic() + float(os.environ.get("MK9_FAKE_PARK", "1.0"))
                self.status("booting")  # on its way back to the main menu
            else:
                self.close_socket()
                self.state = "lost"  # in the middle of a match: the game cannot get back by itself
                self.status("booting")
        elif verb == "start":
            if self.state != "ready":
                self.status("error not at the hold point")
            else:
                self.begin(keys)
        elif verb == "rematch":
            if self.state != "continue":
                self.status("error no match to repeat")
            else:
                again = dict(self.start)
                again.update({k: v for k, v in keys.items() if k in ("match", "seed")})
                select = keys.get("select") == "1" or keys.get("p1") == "-" or keys.get("p2") == "-"
                log("rematch: " + ("both pick again" if select else "the same fighters"))
                self.rebegin(again)
        else:
            self.status("error bad command")
        return True

    def rebegin(self, keys: dict[str, str]) -> None:
        """REMATCH / SELECT A NEW FIGHTER from CONTINUE?: the same peers, the socket kept."""
        self.start = keys
        self.match = keys.get("match", self.match)
        self.started_at = time.monotonic()
        self.barrier += 1
        self.state = "playing" if self.start.get("watch") != "1" else "watching"
        self.connected_at = time.monotonic()
        self.status(f"in_match match={self.match}")

    def pump_network(self, now: float) -> None:
        if self.sock is None:
            return
        if now - self.last_send >= 0.1:
            self.sent += 1
            hb = {"type": "hb", "match": self.match, "side": self.slot() + 1, "slot": self.slot(), "n": self.sent}
            for peer in self.peers:
                try:
                    self.sock.sendto(json.dumps(hb).encode(), peer)
                except OSError:
                    pass
            self.last_send = now
            if self.state == "playing":
                self.feed_write(feed_packet(FEED_OWN, self.slot(), self.barrier, struct.pack("<IB", self.sent, 1) + b"\0" * 8))
        while True:
            try:
                data, _ = self.sock.recvfrom(2048)
            except (BlockingIOError, ConnectionResetError, OSError):
                break
            try:
                msg = json.loads(data)
            except ValueError:
                continue
            if msg.get("type") == "hb":  # the peer's (also its last match's, while it is at CONTINUE?)
                self.last_peer = now

    def step(self, now: float) -> None:
        if self.state == "connecting":
            self.pump_network(now)
            if self.last_peer:
                self.state = "playing"
                self.connected_at = now
                self.status(f"in_match match={self.match}")
            elif now - self.started_at > 10:
                self.close_socket()
                self.state = "over"
                self.status("error no connection")
        elif self.state == "playing":
            self.pump_network(now)
            if now - self.last_peer > 3:
                self.close_socket()
                self.state = "over"
                self.status(f"disconnected match={self.match}")
            elif self.exit_code is not None and now - self.connected_at >= self.match_seconds / 2:
                log(f"ending by itself mid-match, exit code {self.exit_code}")
                os._exit(self.exit_code)
            elif now - self.connected_at >= self.match_seconds:
                self.finish()
        elif self.state == "watching":
            if self.feed is not None:
                try:
                    if self.feed.recv(65536) == b"":
                        self.close_feed()
                except (BlockingIOError, OSError):
                    pass
            if now - self.connected_at >= self.match_seconds:
                self.finish()
        elif self.state in ("continue", "over"):
            self.pump_network(now)  # keep answering the peer until park
        elif self.state == "parking" and now >= self.ready_at:
            self.state = "ready"
            self.status("ready")

    def run(self) -> int:
        self.status("booting")
        time.sleep(float(os.environ.get("MK9_FAKE_BOOT", "1.5")))
        self.state = "ready"
        self.status("ready")
        while True:
            line = take_command(self.cmd_path)
            if line and not self.handle(line):
                log("bye")
                return 0
            self.step(time.monotonic())
            time.sleep(0.02)


def main() -> int:
    global LOG_FILE
    args = {}
    for a in sys.argv[1:]:
        if a.startswith("--"):
            k, _, v = a[2:].partition("=")
            args[k] = v if v else "true"
    LOG_FILE = args.get("log_file") or None
    log("command line: " + " ".join(sys.argv[1:]))
    for name in ("MK9_LINK", "MK9_INSTANCE", "MK9_SESSION_DIR", "MK9_NET_LOG", "MK9_CONTROLS"):
        log(f"{name}={os.environ.get(name, '')}")
    if os.environ.get("MK9_LINK") != "1":
        log("single player: the game's own menus (nothing to do here); close me to stop")
        while True:
            time.sleep(1)
    status_path, cmd_path = os.environ.get("MK9_STATUS"), os.environ.get("MK9_CMD")
    if not status_path or not cmd_path:
        print("run me from the MK9 launcher (MK9_STATUS and MK9_CMD are not set)", file=sys.stderr)
        return 2
    return FakeGame(status_path, cmd_path).run()


if __name__ == "__main__":
    sys.exit(main())
