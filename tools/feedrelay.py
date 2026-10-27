"""A stand-in for the launcher's spectator feed (FeedLink) on one PC, for tests: the players' games
connect to --players-port and write their feed (u16 length + packet, see mk9/net/netplay.cpp); all
of it goes, in order, to every game connected to --watch-port, and a spectator that connects late
gets everything from the start first.
Usage: feedrelay.py --players-port 7201 --watch-port 7202"""
import argparse
import asyncio

log = bytearray()
watchers: set[asyncio.StreamWriter] = set()


async def player(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    peer = writer.get_extra_info("peername")
    print(f"player feed from {peer}", flush=True)
    try:
        while True:
            data = await reader.read(65536)
            if not data:
                break
            log.extend(data)
            for w in list(watchers):
                try:
                    w.write(data)
                except Exception:
                    watchers.discard(w)
    finally:
        print(f"player feed {peer} closed ({len(log)} bytes so far)", flush=True)
        writer.close()


async def watcher(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    peer = writer.get_extra_info("peername")
    print(f"spectator {peer} connected, sending {len(log)} bytes first", flush=True)
    writer.write(bytes(log))
    watchers.add(writer)
    try:
        while await reader.read(4096):
            pass
    finally:
        watchers.discard(writer)
        writer.close()


async def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--players-port", type=int, default=7201)
    ap.add_argument("--watch-port", type=int, default=7202)
    a = ap.parse_args()
    s1 = await asyncio.start_server(player, "127.0.0.1", a.players_port)
    s2 = await asyncio.start_server(watcher, "127.0.0.1", a.watch_port)
    print(f"relay: players {a.players_port}, spectators {a.watch_port}", flush=True)
    async with s1, s2:
        await asyncio.gather(s1.serve_forever(), s2.serve_forever())


if __name__ == "__main__":
    asyncio.run(main())
