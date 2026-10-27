"""MK9 rules: the netcode both games play with, a match ended in the launcher without a result, the MK9 spectator
feed, and King of the Hill lobbies."""

from __future__ import annotations

import asyncio
import base64
import struct

import pytest

from server.client import ServerError

from .conftest import queue_msg, run_to_go


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def feed_packet(kind: int, slot: int, payload: bytes = b"", magic: int = 0x4E394B4D) -> bytes:
    """One packet of a game's spectator feed after its u16 length: u32 magic "MK9N", u8 type, u8 slot, u16 match."""
    body = struct.pack("<IBBH", magic, kind, slot, 1) + payload
    return struct.pack("<H", len(body)) + body


async def queue_pair(server, connect, udp_peers, names, *, netcodes=(None, None), mode="versus", ranked=False):
    conns = [await connect(n) for n in names]
    udps = [await udp_peers(), await udp_peers()]
    for conn, netcode in zip(conns, netcodes):
        msg = queue_msg(mode=mode, ranked=ranked, character="")
        if netcode is not None:
            msg["netcode"] = netcode
        await conn.request(msg)
    gos = await run_to_go(server, list(zip(conns, udps)))
    return conns, udps, gos


@pytest.mark.parametrize("netcodes,expected", [
    (("rollback", "rollback"), "rollback"),
    (("rollback", "lockstep"), "lockstep"),
    ((None, "rollback"), "lockstep"),  # a launcher that does not say plays lockstep
    ((None, None), "lockstep"),
])
async def test_rollback_only_when_both_want_it(server, connect, udp_peers, netcodes, expected):
    _, _, gos = await queue_pair(server, connect, udp_peers, ["Scorpion", "SubZero"], netcodes=netcodes)
    assert [go["netcode"] for go in gos] == [expected, expected]
    assert [(go["p1"], go["p2"]) for go in gos] == [("-", "-"), ("-", "-")]  # picked on the game's select screen


async def test_bad_netcode(server, connect):
    a = await connect("Ermac")
    with pytest.raises(ServerError) as exc:
        await a.request({**queue_msg(), "netcode": "delay-based"})
    assert exc.value.code == "bad_request"


async def test_ended_without_a_result_does_not_count_and_the_series_goes_on(server, connect, udp_peers):
    (a, b), _, gos = await queue_pair(server, connect, udp_peers, ["Kitana", "Mileena"])
    match = gos[0]["match"]
    # one player ends the match in the launcher (the game had no result line): over for both at once
    await a.request({"type": "result", "match": match, "outcome": "ended"})
    ra, rb = await a.expect("match_result", match=match), await b.expect("match_result", match=match)
    assert ra["status"] == rb["status"] == "no_contest" and ra["left"] is None and ra["winner"] is None
    # the other's game lost this one meanwhile: a late report changes nothing
    late = await b.request({"type": "result", "match": match, "outcome": "disconnected"})
    assert late["type"] == "result_ack"
    sa = await a.expect("series", state="between")
    assert (sa["played"], sa["you"], sa["them"], sa["same_fighters"]) == (1, 0, 0, False)
    for conn in (a, b):
        prof = await conn.request({"type": "profile"})
        assert prof["stats"] == {}
    assert server.db.match_row(match)["status"] == "no_contest"


async def test_ended_in_ranked_moves_no_elo(server, connect, udp_peers):
    (a, b), _, gos = await queue_pair(server, connect, udp_peers, ["Jax", "Sonya"], ranked=True)
    match = gos[0]["match"]
    await b.request({"type": "result", "match": match, "outcome": "ended"})
    assert (await a.expect("match_result", match=match))["status"] == "no_contest"
    sa = await a.expect("series", state="between")
    assert sa["ranked"] and (sa["you"], sa["them"]) == (0, 0)
    # leaving a set where nothing counted is void: no rating change
    await a.request({"type": "series_ready", "series": sa["series"], "ready": False})
    over = await b.expect("series", state="over")
    assert over["outcome"] == "void" and "elo" not in over


async def test_tag_and_tyl_play_casual(server, connect, udp_peers):
    _, _, gos = await queue_pair(server, connect, udp_peers, ["Cyrax", "Sektor"], mode="tyl")
    assert gos[0]["mode"] == "tyl" and gos[0]["ranked"] is False
    _, _, gos = await queue_pair(server, connect, udp_peers, ["Kano", "Kabal"], mode="tag", ranked=True)
    assert gos[0]["mode"] == "tag" and gos[0]["ranked"] is True


def lobby_msg(**kw):
    return {"type": "lobby_create", "name": "Hill", "game": "mk9", "mode": "versus", "password": "", "rounds": 3,
            "region": "eu", "character": "", **kw}


async def ready_all(conns, **extra):
    for conn in conns:
        await conn.request({"type": "lobby_ready", "ready": True, **extra})
    await asyncio.sleep(0.2)  # every member has every push by now
    for conn in conns:
        conn.take_all("lobby")


async def lobby_match(server, host, pairs):
    """The host starts the lobby's match between the two (conn, udp) pairs; returns {side: (conn, go)}."""
    await host.request({"type": "lobby_start"})
    gos = await run_to_go(server, pairs)
    return {go["side"]: (conn, go) for (conn, _), go in zip(pairs, gos)}


async def report_win(sides, winner):
    match = sides[1][1]["match"]
    rounds = (2, 0) if winner == 1 else (0, 2)
    for conn, _ in sides.values():
        await conn.request({"type": "result", "match": match, "winner": winner, "p1_rounds": rounds[0],
                            "p2_rounds": rounds[1]})
    for conn, _ in sides.values():
        assert (await conn.expect("match_result", match=match))["status"] == "agreed"


async def lobby_state(conn, pred=None):
    return (await conn.expect("lobby", pred=pred))["lobby"]


def line(state):
    return [m["name"] for m in state["members"] if not m["spectator"]]


async def test_king_of_the_hill(server, connect, udp_peers):
    names = ["Raiden", "Kenshi", "Nightwolf"]
    conns = [await connect(n) for n in names]
    udps = [await udp_peers() for _ in names]
    host = conns[0]
    created = (await host.request(lobby_msg(koth=True)))["lobby"]
    assert created["koth"] is True and created["max_players"] == 8 and created["king"] is None
    code = created["id"]
    listed = (await conns[1].request({"type": "lobby_list", "game": "mk9"}))["lobbies"]
    assert listed[0]["koth"] is True and listed[0]["max_players"] == 8
    for conn in conns[1:]:
        await conn.request({"type": "lobby_join", "lobby": code})
    peer = dict(zip(names, zip(conns, udps)))
    await ready_all(conns, netcode="rollback")

    # the first two in line fight, the first as P1; the third waits
    sides = await lobby_match(server, host, [peer["Raiden"], peer["Kenshi"]])
    assert (sides[1][1]["p1_name"], sides[1][1]["p2_name"]) == ("Raiden", "Kenshi")
    assert sides[1][1]["netcode"] == "rollback"
    # a fourth player joins the line while the match runs
    late = await connect("Kai")
    joined = (await late.request({"type": "lobby_join", "lobby": code}))["lobby"]
    assert line(joined) == ["Raiden", "Kenshi", "Nightwolf", "Kai"] and joined["state"] == "in_match"
    conns.append(late)
    peer["Kai"] = (late, await udp_peers())

    # the challenger wins: the hill is theirs, the loser goes to the end of the line
    await report_win(sides, 2)
    state = await lobby_state(host, pred=lambda m: m["lobby"]["state"] != "in_match")
    assert line(state) == ["Kenshi", "Nightwolf", "Kai", "Raiden"]
    assert (state["king"], state["streak"]) == ("Kenshi", 1)
    assert not any(m["ready"] for m in state["members"])

    await ready_all(conns)
    sides = await lobby_match(server, host, [peer["Kenshi"], peer["Nightwolf"]])
    assert (sides[1][1]["p1_name"], sides[1][1]["p2_name"]) == ("Kenshi", "Nightwolf")
    await report_win(sides, 1)  # the king holds
    state = await lobby_state(host, pred=lambda m: m["lobby"]["state"] != "in_match")
    assert line(state) == ["Kenshi", "Kai", "Raiden", "Nightwolf"]
    assert (state["king"], state["streak"]) == ("Kenshi", 2)

    # a match that does not count keeps the king; the challenger goes to the end
    await ready_all(conns)
    sides = await lobby_match(server, host, [peer["Kenshi"], peer["Kai"]])
    match = sides[1][1]["match"]
    await sides[1][0].request({"type": "result", "match": match, "outcome": "desync", "frame": 77})
    await sides[2][0].request({"type": "result", "match": match, "outcome": "desync", "frame": 77})
    state = await lobby_state(host, pred=lambda m: m["lobby"]["state"] != "in_match")
    assert line(state) == ["Kenshi", "Raiden", "Nightwolf", "Kai"]
    assert (state["king"], state["streak"]) == ("Kenshi", 2)

    # a king who ends the match in the launcher gives up the hill's place: to the end of the line
    await ready_all(conns)
    sides = await lobby_match(server, host, [peer["Kenshi"], peer["Raiden"]])
    match = sides[1][1]["match"]
    await sides[1][0].request({"type": "result", "match": match, "outcome": "ended"})
    state = await lobby_state(host, pred=lambda m: m["lobby"]["state"] != "in_match")
    assert line(state) == ["Raiden", "Nightwolf", "Kai", "Kenshi"]
    assert (state["king"], state["streak"]) == (None, 0)  # nobody holds the hill until the next win

    # the next win takes the hill again
    await ready_all(conns)
    sides = await lobby_match(server, host, [peer["Raiden"], peer["Nightwolf"]])
    await report_win(sides, 2)
    state = await lobby_state(host, pred=lambda m: m["lobby"]["state"] != "in_match")
    assert line(state) == ["Nightwolf", "Kai", "Kenshi", "Raiden"]
    assert (state["king"], state["streak"]) == ("Nightwolf", 1)

    # the king leaves: the next winner takes the hill
    await peer["Nightwolf"][0].request({"type": "lobby_leave"})
    state = await lobby_state(host, pred=lambda m: len(m["lobby"]["members"]) == 3)
    assert line(state) == ["Kai", "Kenshi", "Raiden"] and state["king"] is None and state["streak"] == 0


async def test_king_of_the_hill_needs_the_first_two_ready(server, connect):
    a, b, c = await connect("Liu"), await connect("Kung"), await connect("Shang")
    code = (await a.request(lobby_msg(koth=True)))["lobby"]["id"]
    for conn in (b, c):
        await conn.request({"type": "lobby_join", "lobby": code})
    await a.request({"type": "lobby_ready", "ready": True})
    await c.request({"type": "lobby_ready", "ready": True})  # the third in line: not enough
    with pytest.raises(ServerError) as exc:
        await a.request({"type": "lobby_start"})
    assert exc.value.code == "not_ready"
    # a plain lobby still holds two players
    d, e, f = await connect("Quan"), await connect("Shao"), await connect("Goro")
    code = (await d.request(lobby_msg()))["lobby"]["id"]
    await e.request({"type": "lobby_join", "lobby": code})
    with pytest.raises(ServerError) as exc:
        await f.request({"type": "lobby_join", "lobby": code})
    assert exc.value.code == "lobby_full"


async def test_spectator_feed_takes_mk9_packets_only(server, connect, udp_peers):
    host, guest, watcher = await connect("Smoke"), await connect("Noob"), await connect("Rain")
    code = (await host.request(lobby_msg()))["lobby"]["id"]
    await guest.request({"type": "lobby_join", "lobby": code})
    await watcher.request({"type": "lobby_join", "lobby": code, "spectate": True})
    for conn in (host, guest):
        await conn.request({"type": "lobby_ready", "ready": True})
    await host.request({"type": "lobby_start"})
    go, _ = await run_to_go(server, [(host, await udp_peers()), (guest, await udp_peers())])
    spec = await watcher.expect("spectate")
    assert spec["match"] == go["match"] and spec["netcode"] == "lockstep" and spec["mode"] == "versus"
    meet = feed_packet(6, 0, struct.pack("<IB", 12345, 1) + b"\0\0\0\0")
    own = feed_packet(7, 0, struct.pack("<IB", 0, 2) + b"\0" * 16)
    host.send({"type": "feed", "match": go["match"], "data": b64(meet + own)})
    assert base64.b64decode((await watcher.expect("spectate_feed"))["data"]) == meet + own
    for bad in (feed_packet(7, 0, magic=0x52444B4D),  # an MK3D game's packet
                feed_packet(1, 0),                    # a netplay packet, not a feed one
                feed_packet(7, 1)):                   # the other player's slot
        with pytest.raises(ServerError) as exc:
            await host.request({"type": "feed", "match": go["match"], "data": b64(bad)})
        assert exc.value.code == "bad_request"


async def agree(conns, match, winner=1):
    rounds = (2, 0) if winner == 1 else (0, 2)
    for conn in conns:
        await conn.request({"type": "result", "match": match, "winner": winner, "p1_rounds": rounds[0],
                            "p2_rounds": rounds[1]})
    for conn in conns:
        assert (await conn.expect("match_result", match=match))["status"] == "agreed"


async def test_series_rematch_from_the_continue_menu(server, connect, udp_peers):
    (a, b), _, gos = await queue_pair(server, connect, udp_peers, ["Jade", "Kitana"])
    await agree((a, b), gos[0]["match"])
    sa = await a.expect("series", state="between")
    # both games still wait together at their CONTINUE? menu: the rematch starts there, with no new setup
    for conn in (a, b):
        await conn.request({"type": "series_ready", "series": sa["series"], "ready": True, "choice": "rematch",
                            "continue": True})
    go2 = [await conn.expect("match_go") for conn in (a, b)]
    assert [g["rematch"] for g in go2] == ["same", "same"] and go2[0]["series"] == sa["series"]
    assert go2[0]["match"] == go2[1]["match"] != gos[0]["match"] and go2[0]["seed"] == go2[1]["seed"]
    assert [(g["side"], g["remote"], g["netcode"]) for g in go2] == [(g["side"], g["remote"], g["netcode"]) for g in gos]
    assert not a.take_all("match_found") and not b.take_all("match_found")

    # either one choosing SELECT: both pick again, still from the CONTINUE? menu
    await agree((a, b), go2[0]["match"], winner=2)
    sa = await a.expect("series", state="between", played=2)
    await a.request({"type": "series_ready", "series": sa["series"], "ready": True, "choice": "rematch", "continue": True})
    await b.request({"type": "series_ready", "series": sa["series"], "ready": True, "choice": "select", "continue": True})
    go3 = [await conn.expect("match_go") for conn in (a, b)]
    assert [g["rematch"] for g in go3] == ["select", "select"]

    # a game that went back to its main menu: the next match is set up as a new one
    await agree((a, b), go3[0]["match"])
    sa = await a.expect("series", state="between", played=3)
    await a.request({"type": "series_ready", "series": sa["series"], "ready": True, "continue": True})
    await b.request({"type": "series_ready", "series": sa["series"], "ready": True})
    found = [await conn.expect("match_found") for conn in (a, b)]
    assert all(f["series"] == sa["series"] and not f["accept_required"] for f in found)


async def test_ranked_set_goes_on_from_the_continue_menu(server, connect, udp_peers):
    (a, b), _, gos = await queue_pair(server, connect, udp_peers, ["Reptile", "Ermac"], ranked=True)
    await agree((a, b), gos[0]["match"])
    sa = await a.expect("series", state="between")
    assert sa["ranked"] and sa["need"] == 2
    for conn in (a, b):
        await conn.request({"type": "series_ready", "series": sa["series"], "ready": True, "continue": True})
    go2 = [await conn.expect("match_go") for conn in (a, b)]
    assert [g["rematch"] for g in go2] == ["select", "select"] and go2[0]["ranked"] is True
    await agree((a, b), go2[0]["match"])  # P1 wins the set 2-0
    over = await a.expect("series", state="over")
    assert over["outcome"] == "won" and over["elo"]


def test_auto_delay_covers_the_one_way_latency():
    from server import protocol as P
    # ceil((rtt / 2 + 8) / 16.67), 2 to 8 frames
    assert [P.auto_delay(rtt) for rtt in (0, 20, 38, 50, 51, 100, 150, 200, 250, 400)] == [2, 2, 2, 2, 3, 4, 5, 7, 8, 8]
    assert P.get_delay({"delay": "auto"}) == P.DELAY_AUTO and P.get_delay({}) == 2 and P.get_delay({"delay": 6}) == 6
    for bad in (7, -2, "fast", True):
        with pytest.raises(P.ProtocolError):
            P.get_delay({"delay": bad})


@pytest.mark.parametrize("delays,expected", [
    (("auto", "auto"), (2, True)),  # loopback: the smallest auto delay
    (("auto", 5), (5, True)),       # the larger of the two players' delays
    ((3, "auto"), (3, True)),
    ((0, 1), (1, False)),
])
async def test_auto_delay_in_match_go(server, connect, udp_peers, delays, expected):
    conns = [await connect(n) for n in ("Sindel", "Sheeva")]
    udps = [await udp_peers(), await udp_peers()]
    for conn, delay in zip(conns, delays):
        await conn.request({**queue_msg(character=""), "delay": delay})
    gos = await run_to_go(server, list(zip(conns, udps)))
    assert [(g["delay"], g["delay_auto"]) for g in gos] == [expected, expected]


async def test_auto_delay_in_a_lobby(server, connect, udp_peers):
    host, guest = await connect("Shang"), await connect("Quan")
    code = (await host.request(lobby_msg()))["lobby"]["id"]
    await guest.request({"type": "lobby_join", "lobby": code})
    await host.request({"type": "lobby_ready", "ready": True, "delay": "auto"})
    await guest.request({"type": "lobby_ready", "ready": True, "delay": "auto"})
    await host.request({"type": "lobby_start"})
    gos = await run_to_go(server, [(host, await udp_peers()), (guest, await udp_peers())])
    assert [(g["delay"], g["delay_auto"]) for g in gos] == [(2, True), (2, True)]
