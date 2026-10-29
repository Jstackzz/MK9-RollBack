"""Lobbies: create (with password), list, join, chat, character, ready, host start, leave."""

from __future__ import annotations

import asyncio

import pytest

from server.client import ServerError

from .conftest import hello_and_accept


def create_msg(**kw):
    return {"type": "lobby_create", "name": "Friday sets", "game": "mk9", "mode": "versus", "password": "",
            "rounds": 3, "region": "eu", "character": "Scorpion", **kw}


async def test_lobby_create_join_ready_start(server, connect, udp_peers):
    host = await connect("Goro")
    guest = await connect("ShaoKahn")
    created = await host.request(create_msg(password="pw-123"))
    lobby = created["lobby"]
    code = lobby["id"]
    assert len(code) == 5 and lobby["locked"] and lobby["state"] == "open"
    assert lobby["members"] == [{"name": "Goro", "character": "Scorpion", "ready": False, "host": True,
                                 "spectator": False, "link": None, "ping": None}]

    listed = await guest.request({"type": "lobby_list", "game": "mk9"})
    assert [(lb["id"], lb["players"], lb["locked"]) for lb in listed["lobbies"]] == [(code, 1, True)]
    assert [lb["id"] for lb in (await guest.request({"type": "lobby_list"}))["lobbies"]] == [code]
    with pytest.raises(ServerError) as exc:
        await guest.request({"type": "lobby_list", "game": "mkd"})  # an MK3D game is no game of this server
    assert exc.value.code == "bad_game"

    with pytest.raises(ServerError) as exc:
        await guest.request({"type": "lobby_join", "lobby": code, "password": "wrong"})
    assert exc.value.code == "bad_lobby_password"
    with pytest.raises(ServerError) as exc:
        await guest.request({"type": "lobby_join", "lobby": "ZZZZZ"})
    assert exc.value.code == "no_such_lobby"
    joined = await guest.request({"type": "lobby_join", "lobby": code.lower(), "password": "pw-123",
                                  "character": "Kenshi"})
    assert [m["name"] for m in joined["lobby"]["members"]] == ["Goro", "ShaoKahn"]
    pushed = await host.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 2)
    assert pushed["lobby"]["state"] == "full"

    third = await connect("Moloch")
    with pytest.raises(ServerError) as exc:
        await third.request({"type": "lobby_join", "lobby": code, "password": "pw-123"})
    assert exc.value.code == "lobby_full"
    with pytest.raises(ServerError) as exc:
        await guest.request({"type": "queue_join", "game": "mk9", "mode": "versus", "region": "eu"})
    assert exc.value.code == "busy"

    # chat reaches both members and is kept in the lobby state
    await guest.request({"type": "lobby_chat", "text": "  good   luck  "})
    chat = await host.expect("lobby_chat")
    assert (chat["from"], chat["text"], chat["lobby"]) == ("ShaoKahn", "good luck", code)
    with pytest.raises(ServerError) as exc:
        await guest.request({"type": "lobby_chat", "text": "spam"})
    assert exc.value.code == "slow_down"

    await guest.request({"type": "lobby_character", "character": "Mileena"})
    with pytest.raises(ServerError) as exc:
        await guest.request({"type": "lobby_start"})
    assert exc.value.code == "not_host"
    await host.request({"type": "lobby_ready", "ready": True})
    with pytest.raises(ServerError) as exc:
        await host.request({"type": "lobby_start"})
    assert exc.value.code == "not_ready"
    state = await guest.request({"type": "lobby_ready", "ready": True})
    assert [m["ready"] for m in state["lobby"]["members"]] == [True, True]
    assert state["lobby"]["members"][1]["character"] == "Mileena"
    assert state["lobby"]["chat"][-1]["text"] == "good luck"

    await host.request({"type": "lobby_start"})
    fh, fg = await host.expect("match_found"), await guest.expect("match_found")
    assert fh["match"] == fg["match"] and fh["lobby"] == code
    assert fh["accept_required"] is False and fh["side"] == 1 and fg["side"] == 2
    assert fg["opponent"]["name"] == "Goro"
    in_match = await guest.expect("lobby", pred=lambda m: m["lobby"]["state"] == "in_match")
    assert in_match["lobby"]["match"] == fh["match"]

    # the rest of the flow is the queue one, without the accept step
    uh, ug = await udp_peers(), await udp_peers()
    await hello_and_accept(server, host, uh, fh)
    await hello_and_accept(server, guest, ug, fg)
    ph, pg = await host.expect("match_peer"), await guest.expect("match_peer")
    rh, rg = await asyncio.gather(
        uh.punch(ph["peer"], match=ph["match"], key=ph["key"], side=1, timeout=5),
        ug.punch(pg["peer"], match=pg["match"], key=pg["key"], side=2, timeout=5),
    )
    host.send({"type": "ping_report", "match": ph["match"], **rh})
    guest.send({"type": "ping_report", "match": pg["match"], **rg})
    gh = await host.expect("match_go")
    assert (gh["p1"], gh["p2"], gh["p1_name"], gh["lobby"]) == ("Scorpion", "Mileena", "Goro", code)
    assert gh["ranked"] is False

    # results finish the match; the lobby opens again with ready flags cleared
    for conn in (host, guest):
        await conn.request({"type": "result", "match": gh["match"], "winner": 2, "p1_rounds": 1, "p2_rounds": 2})
    assert (await host.expect("match_result"))["status"] == "agreed"
    reopened = await host.expect("lobby", pred=lambda m: m["lobby"]["state"] == "full" and m["lobby"]["match"] is None)
    assert [m["ready"] for m in reopened["lobby"]["members"]] == [False, False]


async def test_host_leaving_hands_over_and_empty_lobby_closes(server, connect):
    host = await connect("Fujin")
    guest = await connect("Rain")
    code = (await host.request(create_msg(name="")))["lobby"]["id"]
    assert server.lobbies[code].name == "Fujin's lobby"
    await guest.request({"type": "lobby_join", "lobby": code})
    left = await host.request({"type": "lobby_leave"})
    assert left == {"type": "lobby_left", "lobby": code, "id": left["id"]}
    update = await guest.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 1)
    assert update["lobby"]["host"] == "Rain" and update["lobby"]["members"][0]["host"] is True
    await guest.close()
    await asyncio.sleep(0.2)
    assert code not in server.lobbies


async def test_members_show_their_connection(server, connect):
    host, guest = await connect("Goro"), await connect("ShaoKahn")
    created = await host.request({**create_msg(), "link": "ethernet", "server_rtt": 25})
    code = created["lobby"]["id"]
    joined = await guest.request({"type": "lobby_join", "lobby": code, "link": "wifi", "server_rtt": 70})
    assert [(m["name"], m["link"], m["ping"]) for m in joined["lobby"]["members"]] == [
        ("Goro", "ethernet", 25), ("ShaoKahn", "wifi", 70)]
    # ready says it again (a launcher that moved to another network); nonsense is unknown, a bad ping is ignored
    ready = await guest.request({"type": "lobby_ready", "ready": True, "link": "pigeon", "server_rtt": 99999})
    assert [(m["link"], m["ping"]) for m in ready["lobby"]["members"]] == [("ethernet", 25), (None, 70)]
    # a ready without them (a launcher up to 0.4.0) keeps what was said
    ready = await guest.request({"type": "lobby_ready", "ready": False})
    assert [(m["link"], m["ping"]) for m in ready["lobby"]["members"]] == [("ethernet", 25), (None, 70)]


async def test_a_dropped_connection_gets_its_lobby_place_back(server, connect):
    host = await connect("Goro")
    guest = await connect("ShaoKahn")
    code = (await host.request(create_msg()))["lobby"]["id"]
    await guest.request({"type": "lobby_join", "lobby": code, "character": "Kenshi"})
    await host.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 2)
    token = guest.account["token"]
    await guest.close()  # the connection drops: the place is kept a while
    await host.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 1)

    back = await connect()
    await back.request({"type": "auth", "token": token})
    pushed = await back.expect("lobby")
    assert [(m["name"], m["character"], m["spectator"]) for m in pushed["lobby"]["members"]] == [
        ("Goro", "Scorpion", False), ("ShaoKahn", "Kenshi", False)]
    await host.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 2)

    # signing out gives the place up, and so does a grace that ran out
    await back.request({"type": "logout"})
    await host.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 1)
    again = await connect("ShaoKahn", register=False)
    await asyncio.sleep(0.2)
    assert [m.session.name for m in server.lobbies[code].members] == ["Goro"]
    await again.request({"type": "lobby_join", "lobby": code})
    server.cfg.lobby_rejoin_grace = 0.0
    await again.close()
    await host.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 1)
    await asyncio.sleep(0.05)
    await connect("ShaoKahn", register=False)
    await asyncio.sleep(0.2)
    assert [m.session.name for m in server.lobbies[code].members] == ["Goro"]


async def test_lobby_starts_by_itself_once_both_are_ready(server, connect):
    """Both players ready begins a countdown (the state's starts_in), un-readying calls it off, and once it has run
    the match is found as a host's lobby_start would have it."""
    server.cfg.lobby_countdown = 0.4
    host, guest = await connect("Goro"), await connect("ShaoKahn")
    code = (await host.request(create_msg()))["lobby"]["id"]
    await guest.request({"type": "lobby_join", "lobby": code, "character": "Kenshi"})
    first = await host.request({"type": "lobby_ready", "ready": True})
    assert first["lobby"]["starts_in"] is None
    state = await guest.request({"type": "lobby_ready", "ready": True})
    assert 0 < state["lobby"]["starts_in"] <= 0.4

    off = await host.request({"type": "lobby_ready", "ready": False})
    assert off["lobby"]["starts_in"] is None
    await asyncio.sleep(0.7)
    assert server.lobbies[code].match is None and not host.take_all("match_found")

    again = await host.request({"type": "lobby_ready", "ready": True})
    assert again["lobby"]["starts_in"] is not None
    fh, fg = await host.expect("match_found"), await guest.expect("match_found")
    assert fh["match"] == fg["match"] and fh["lobby"] == code and fh["accept_required"] is False
    assert (fh["side"], fg["side"]) == (1, 2)
    in_match = await guest.expect("lobby", pred=lambda m: m["lobby"]["state"] == "in_match")
    assert in_match["lobby"]["match"] == fh["match"] and in_match["lobby"]["starts_in"] is None


async def test_a_king_of_the_hill_lobby_counts_down_for_the_first_two_in_line(server, connect):
    """King of the Hill: the countdown waits for the first two in line (the king, or the first, as P1), whoever
    else is ready; the third in line readying alone starts nothing."""
    server.cfg.lobby_countdown = 0.3
    a, b, c = await connect("Liu"), await connect("Kung"), await connect("Shang")
    code = (await a.request(create_msg(koth=True)))["lobby"]["id"]
    for conn in (b, c):
        await conn.request({"type": "lobby_join", "lobby": code})
    assert (await a.request({"type": "lobby_ready", "ready": True}))["lobby"]["starts_in"] is None
    assert (await c.request({"type": "lobby_ready", "ready": True}))["lobby"]["starts_in"] is None
    await asyncio.sleep(0.5)
    assert server.lobbies[code].match is None
    assert (await b.request({"type": "lobby_ready", "ready": True}))["lobby"]["starts_in"] is not None
    fa, fb = await a.expect("match_found"), await b.expect("match_found")
    assert (fa["side"], fb["side"]) == (1, 2) and fa["match"] == fb["match"]
    assert not c.take_all("match_found")


async def test_a_lobby_named_after_its_host_takes_the_next_hosts_name(server, connect):
    """The host leaves: the lobby named after them (the launcher's default) is named after the new host, so the list
    never shows two lobbies named after one player; a lobby its host named stays as it is."""
    host, guest = await connect("Goro"), await connect("ShaoKahn")
    code = (await host.request(create_msg(name="Goro's lobby")))["lobby"]["id"]
    await guest.request({"type": "lobby_join", "lobby": code, "character": "Kenshi"})
    await host.request({"type": "lobby_leave"})
    state = await guest.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 1)
    assert (state["lobby"]["host"], state["lobby"]["name"]) == ("ShaoKahn", "ShaoKahn's lobby")

    third = await connect("Moloch")
    code2 = (await third.request(create_msg(name="Friday sets")))["lobby"]["id"]
    fourth = await connect("Kitana")
    await fourth.request({"type": "lobby_join", "lobby": code2, "character": "Kenshi"})
    await third.request({"type": "lobby_leave"})
    kept = await fourth.expect("lobby", pred=lambda m: len(m["lobby"]["members"]) == 1)
    assert (kept["lobby"]["host"], kept["lobby"]["name"]) == ("Kitana", "Friday sets")


async def test_online_counts_the_players_of_the_game(server, connect):
    """The game's players for the matchmaking pages: searching, in its matches, in its lobbies (spectators too), and
    playing, any of those, each player once."""
    from .conftest import queue_msg

    searching, tag, host, spectator, p1, p2 = [
        await connect(n) for n in ("Kitana", "Tanya", "Sindel", "Jade", "Frost", "Sareena")]
    await searching.request(queue_msg())
    await tag.request(queue_msg(mode="tag"))
    code = (await host.request(create_msg()))["lobby"]["id"]
    await spectator.request({"type": "lobby_join", "lobby": code, "spectate": True})
    await p1.request(queue_msg(mode="tyl"))
    await p2.request(queue_msg(mode="tyl"))
    await p1.expect("match_found")
    online = await searching.request({"type": "online"})
    assert online["games"] == {"mk9": {"playing": 6, "searching": 2, "matches": 2, "lobbies": 2}}
    assert online["queues"] == {"mk9/tag/casual": 1, "mk9/versus/casual": 1}
    # a lobby left empty is gone from the count, and the push says so
    searching.take_all("online")  # the pushes so far
    await spectator.request({"type": "lobby_leave"})
    await host.request({"type": "lobby_leave"})
    pushed = await searching.expect("online", timeout=5, pred=lambda m: "id" not in m and
                                    m["games"]["mk9"]["lobbies"] == 0)
    assert pushed["games"]["mk9"] == {"playing": 4, "searching": 2, "matches": 2, "lobbies": 0}


async def test_online_lists_who_is_searching(server, connect):
    """The matchmaking pages show who searches, for which queue, the longest waiting first; a search that ends leaves
    the list, and every connection is told."""
    from .conftest import queue_msg

    kitana, tanya, watcher = [await connect(n) for n in ("Kitana", "Tanya", "Jade")]
    await kitana.request(queue_msg())
    await tanya.request(queue_msg(mode="tag", ranked=True))
    online = await watcher.request({"type": "online"})
    assert online["searchers"] == [
        {"name": "Kitana", "game": "mk9", "queue": "mk9/versus/casual"},
        {"name": "Tanya", "game": "mk9", "queue": "mk9/tag/ranked"},
    ]
    watcher.take_all("online")  # the pushes so far
    await kitana.request({"type": "queue_leave"})
    pushed = await watcher.expect("online", timeout=5, pred=lambda m: "id" not in m and len(m["searchers"]) == 1)
    assert pushed["searchers"] == [{"name": "Tanya", "game": "mk9", "queue": "mk9/tag/ranked"}]
