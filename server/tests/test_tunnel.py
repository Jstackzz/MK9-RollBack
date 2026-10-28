"""Tunnels (a match through a player's TLS connection when their network lets no UDP through) and crash reports."""

from __future__ import annotations

import asyncio
import base64
import os
import zlib

import pytest

from server.client import ServerError, TunnelPeer

from .conftest import hello_and_accept, queue_msg


async def found_both(a, b):
    await a.request(queue_msg())
    await b.request(queue_msg())
    return await a.expect("match_found"), await b.expect("match_found")


def relay_of(server) -> dict:
    return {"public": f"127.0.0.1:{server.udp_port}", "lan": ""}


async def tunnel_punch(server, peer: TunnelPeer, mp: dict) -> dict:
    return await peer.punch(match=mp["match"], key=mp["key"], side=mp["side"],
                            server_udp=f"127.0.0.1:{server.udp_port}", timeout=5)


async def test_a_player_without_udp_plays_through_its_connection(server, connect, udp_peers):
    a, b = await connect("LiuKang"), await connect("KungLao", tunnel=True)
    assert b.welcome["tunnel"] is True
    ua = await udp_peers()
    fa, fb = await found_both(a, b)
    await hello_and_accept(server, a, ua, fa)
    b.send({"type": "match_accept", "match": fb["match"]})
    b.send({"type": "tunnel", "match": fb["match"]})  # no hello_ack came back: UDP is blocked there
    pa, pb = await a.expect("match_peer"), await b.expect("match_peer")
    assert pa["relay"] and not pa["tunnel"]
    assert pb["relay"] and pb["tunnel"] and pa["key"] == pb["key"]
    tb = TunnelPeer(b)
    try:
        ra, rb = await asyncio.gather(
            ua.punch(relay_of(server), match=pa["match"], key=pa["key"], side=pa["side"], timeout=5),
            tunnel_punch(server, tb, pb),
        )
        assert ra["ok"] and rb["ok"], (ra, rb)
        a.send({"type": "ping_report", "match": pa["match"], **ra, "via": "relay", "key": pa["key"]})
        b.send({"type": "ping_report", "match": pb["match"], **rb})
        ga, gb = await a.expect("match_go"), await b.expect("match_go")
        assert ga["seed"] == gb["seed"] and gb["remote"] == f"127.0.0.1:{server.udp_port}"
        # the game's packets: UDP on one side, the connection on the other, byte for byte both ways
        assert ua.transport is not None
        packet = b"MK9N\x03\x00\x01\x00" + bytes(range(60))
        ua.transport.sendto(packet, ("127.0.0.1", server.udp_port))
        assert await asyncio.wait_for(tb.raw.get(), 2) == packet
        tb.send(packet[::-1], packet)
        assert (await asyncio.wait_for(ua.raw.get(), 2))[0] == packet[::-1]
        assert (await asyncio.wait_for(ua.raw.get(), 2))[0] == packet
    finally:
        tb.close()


async def test_no_udp_by_the_deadline_goes_through_the_connections(server, connect):
    server.cfg.udp_timeout = 0.5
    a, b = await connect("Jade", tunnel=True), await connect("Tanya", tunnel=True)
    fa, fb = await found_both(a, b)
    for conn, found in ((a, fa), (b, fb)):
        conn.send({"type": "match_accept", "match": found["match"]})
    pa, pb = await a.expect("match_peer"), await b.expect("match_peer")
    assert pa["tunnel"] and pb["tunnel"]
    ta, tb = TunnelPeer(a), TunnelPeer(b)
    try:
        ra, rb = await asyncio.gather(tunnel_punch(server, ta, pa), tunnel_punch(server, tb, pb))
        assert ra["ok"] and rb["ok"], (ra, rb)
        a.send({"type": "ping_report", "match": pa["match"], **ra})
        b.send({"type": "ping_report", "match": pb["match"], **rb})
        await a.expect("match_go")
        await b.expect("match_go")
        ta.send(b"MK9N\x07\x00\x01\x00abc")
        assert await asyncio.wait_for(tb.raw.get(), 2) == b"MK9N\x07\x00\x01\x00abc"
        tb.send(b"MK9N\x07\x01\x01\x00xyz")
        assert await asyncio.wait_for(ta.raw.get(), 2) == b"MK9N\x07\x01\x01\x00xyz"
    finally:
        ta.close()
        tb.close()


async def test_a_launcher_that_cannot_tunnel_still_gets_no_udp(server, connect):
    server.cfg.udp_timeout = 0.5
    a, b = await connect("Sindel"), await connect("Kitana")  # launchers before 0.4.0: no tunnel in their hello
    fa, fb = await found_both(a, b)
    for conn, found in ((a, fa), (b, fb)):
        conn.send({"type": "match_accept", "match": found["match"]})
    ca, cb = await a.expect("match_cancelled"), await b.expect("match_cancelled")
    assert ca["reason"] == cb["reason"] == "no_udp"


async def test_a_failed_relay_moves_to_the_connections(server, connect, udp_peers):
    a, b = await connect("Smoke", tunnel=True), await connect("Noob", tunnel=True)
    ua, ub = await udp_peers(), await udp_peers()
    fa, fb = await found_both(a, b)
    await hello_and_accept(server, a, ua, fa)
    await hello_and_accept(server, b, ub, fb)
    pa, pb = await a.expect("match_peer"), await b.expect("match_peer")
    for conn, mp in ((a, pa), (b, pb)):  # no direct route
        conn.send({"type": "ping_report", "match": mp["match"], "ok": False, "rtt_ms": 0, "key": mp["key"]})
    ra, rb = await a.expect("match_peer"), await b.expect("match_peer")
    assert ra["relay"] and not ra["tunnel"]
    for conn, mp in ((a, ra), (b, rb)):  # the relay's UDP fails too
        conn.send({"type": "ping_report", "match": mp["match"], "ok": False, "rtt_ms": 0, "key": mp["key"]})
    xa, xb = await a.expect("match_peer"), await b.expect("match_peer")
    assert xa["tunnel"] and xb["tunnel"] and xa["key"] not in (pa["key"], ra["key"])
    # a report of the relay's attempt that comes late is left aside
    a.send({"type": "ping_report", "match": ra["match"], "ok": False, "rtt_ms": 0, "key": ra["key"]})
    ta, tb = TunnelPeer(a), TunnelPeer(b)
    try:
        ga_report, gb_report = await asyncio.gather(tunnel_punch(server, ta, xa), tunnel_punch(server, tb, xb))
        a.send({"type": "ping_report", "match": xa["match"], **ga_report})
        b.send({"type": "ping_report", "match": xb["match"], **gb_report})
        await a.expect("match_go")
        await b.expect("match_go")
        assert not a.take_all("match_cancelled")
    finally:
        ta.close()
        tb.close()


async def test_tunnel_while_punching_starts_the_relay_at_once(server, connect, udp_peers):
    a, b = await connect("Kabal", tunnel=True), await connect("Stryker", tunnel=True)
    ua, ub = await udp_peers(), await udp_peers()
    fa, fb = await found_both(a, b)
    await hello_and_accept(server, a, ua, fa)
    await hello_and_accept(server, b, ub, fb)
    pa, pb = await a.expect("match_peer"), await b.expect("match_peer")
    assert not pa.get("relay")
    # b's hellos reached the server, but nothing comes back to it: it asks for the tunnel while a punches
    b.send({"type": "tunnel", "match": pb["match"]})
    ra, rb = await a.expect("match_peer"), await b.expect("match_peer")
    assert ra["relay"] and not ra["tunnel"] and rb["tunnel"] and ra["key"] != pa["key"]
    a.send({"type": "ping_report", "match": pa["match"], "ok": False, "rtt_ms": 0, "key": pa["key"]})  # the old one
    tb = TunnelPeer(b)
    try:
        r1, r2 = await asyncio.gather(
            ua.punch(relay_of(server), match=ra["match"], key=ra["key"], side=ra["side"], timeout=5),
            tunnel_punch(server, tb, rb),
        )
        a.send({"type": "ping_report", "match": ra["match"], **r1, "via": "relay", "key": ra["key"]})
        b.send({"type": "ping_report", "match": rb["match"], **r2})
        await a.expect("match_go")
        await b.expect("match_go")
    finally:
        tb.close()


def packed_report(text: bytes) -> str:
    """As the launcher packs one: Qt's qCompress (the size, 4 bytes big-endian, then zlib), in base64."""
    return base64.b64encode(len(text).to_bytes(4, "big") + zlib.compress(text, 9)).decode("ascii")


async def send_report(conn, report_id: str, text: bytes, part_chars: int = 9000) -> None:
    packed = packed_report(text)
    parts = [packed[i:i + part_chars] for i in range(0, len(packed), part_chars)]
    for i, part in enumerate(parts):
        conn.send({"type": "crash_report", "report": report_id, "part": i, "parts": len(parts), "data": part})


async def test_a_crash_report_is_kept(server, connect, tmp_path):
    server.cfg.crash_dir = str(tmp_path / "crashes")
    c = await connect("Cyrax")
    text = b"MK9 Online crash report\n" + os.urandom(20000).hex().encode()  # about 3 parts
    await send_report(c, "0123456789abcdef", text)
    ack = await c.expect("crash_report_ack")
    assert ack["report"] == "0123456789abcdef"
    files = os.listdir(tmp_path / "crashes")
    assert len(files) == 1 and "-Cyrax-01234567" in files[0]
    body = (tmp_path / "crashes" / files[0]).read_bytes()
    assert body.endswith(text) and b"from Cyrax" in body


async def test_crash_reports_are_limited_and_checked(server, connect, tmp_path):
    server.cfg.crash_dir = str(tmp_path / "crashes")
    server.cfg.crash_rate = 2
    server.cfg.crash_keep = 1
    guest = await connect()  # not signed in: a crash at start may come before an account
    for rid in ("aaaaaaaa00000001", "cccccccc00000002"):
        await send_report(guest, rid, b"report " + rid.encode())
        await guest.expect("crash_report_ack", report=rid)
    assert len(os.listdir(tmp_path / "crashes")) == 1  # the newest crash_keep stay
    with pytest.raises(ServerError) as limited:
        await guest.request({"type": "crash_report", "report": "aaaaaaaa00000003", "part": 0, "parts": 1,
                             "data": packed_report(b"one more")})
    assert limited.value.code == "rate_limited"
    server.crash_sent.clear()
    with pytest.raises(ServerError) as bad:
        await guest.request({"type": "crash_report", "report": "bbbbbbbb00000001", "part": 0, "parts": 1,
                             "data": base64.b64encode(b"\x00\x00\x00\x10not zlib at all").decode()})
    assert bad.value.code == "bad_request"
    with pytest.raises(ServerError) as huge:
        await guest.request({"type": "crash_report", "report": "bbbbbbbb00000002", "part": 0, "parts": 1,
                             "data": packed_report(b"x" * (3 << 20))})
    assert huge.value.code == "bad_request"
