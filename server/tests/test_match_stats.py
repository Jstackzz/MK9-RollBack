"""Match stats (docs/PROTOCOL.md, "Match stats"): what each match was like on its players' PCs, kept beside the match,
logged, and shown by `python -m server.matchlog`."""

from __future__ import annotations

import json

import pytest

from server import matchlog, matchstats
from server.client import ServerError

from .test_results import report, setup_match


def game_stats(**over) -> dict:
    """What a game writes (mk9/src/mk9/net/netplay.cpp, MatchStatsJson), plus the launcher's own fields."""
    st = {
        "v": 1, "match": "x", "end": "result", "netcode": "rollback", "delay": 4, "secs": 151.2, "fight_secs": 120.0,
        "frames": 7190, "late": 3, "stalls": 0, "longest_ms": 41.2, "rollbacks": 310, "rerun": 640,
        "depth": [200, 96, 12, 2], "rollback_ms": 1800.5, "rollback_longest_ms": 21.3, "waits": 0, "wait_ms": 0,
        "held": 12, "their_fps": 59.8, "my_fps": 60.0, "round_trip": 9.4, "cpu": "AMD Ryzen 7 9800X3D",
        "gpu": "NVIDIA GeForce RTX 4070", "per_second": [[0, 0, 0, 0, 0, 0]] * 30 + [[60, 0, 4, 2, 0, 0]] * 119
        + [[57, 3, 20, 5, 0, 0]],
    }
    st.update(over)
    return st


def stats_msg(match, **over):
    return {"type": "match_stats", "game": "mk9", "match": match, "stats": game_stats(**over)}


async def test_both_players_stats_are_kept_and_logged(server, connect, udp_peers, caplog):
    sides, match = await setup_match(server, connect, udp_peers, ["Sonya", "Kano"], ranked=False)
    (c1, go), (c2, _) = sides[1], sides[2]
    await report(c1, match, 1, 2, 0)
    await report(c2, match, 1, 2, 0)
    with caplog.at_level("INFO", logger="mk9.server"):
        ack = await c1.request(stats_msg(match))
        assert (ack["type"], ack["match"]) == ("match_stats_ack", match)
        # the other's PC held to 40 frames a second: a fight that stuttered
        await c2.request(stats_msg(match, frames=4800, late=900, stalls=40, longest_ms=88.0, their_fps=60.0,
                                   my_fps=40.0, wait_ms=4100.0, waits=230, per_second=[[40, 20, 0, 0, 30, 0]] * 120))
    said = [r.getMessage() for r in caplog.records if "STATS" in r.getMessage()]
    assert len(said) == 2
    assert said[0].startswith(f"STATS match={match} {go['p1_name']} (side 1, launcher ")
    assert ": smooth; 59.9 fps over 2:00 of fight, 3 frames late" in said[0]
    assert "the other at 60 fps, this PC at 60" in said[0] and "delay 4, rollback, ended result" in said[0]
    assert ": stutter; 40.0 fps" in said[1]
    rows = server.db.match_stats(match)
    names = {r["id"]: r["name"] for r in server.db.conn.execute("SELECT id, name FROM accounts")}
    assert [(r["side"], names[r["account_id"]]) for r in rows] == [(1, go["p1_name"]), (2, go["p2_name"])]
    kept = json.loads(rows[0]["stats"])
    assert kept["late"] == 3 and kept["depth"] == [200, 96, 12, 2] and kept["gpu"] == "NVIDIA GeForce RTX 4070"
    assert "match" not in kept and "per_second" not in kept  # the match is the row's; the lines a second apart
    assert len(json.loads(rows[0]["per_second"])) == 150
    # sent again (a launcher that retried): the later one stands
    await c1.request(stats_msg(match, late=4))
    assert json.loads(server.db.match_stats(match)[0]["stats"])["late"] == 4


async def test_stats_only_for_the_players_own_recent_match(server, connect, udp_peers):
    sides, match = await setup_match(server, connect, udp_peers, ["Jax", "Kabal"], ranked=False)
    (c1, _), _ = sides[1], sides[2]
    other = await connect("Stryker")
    for conn, msg, code in [
        (other, stats_msg(match), "no_such_match"),  # not their match
        (c1, stats_msg("0123456789ab"), "no_such_match"),
        (c1, {**stats_msg(match), "game": "mk3d"}, "bad_request"),
        (c1, {**stats_msg(match), "stats": "fast"}, "bad_request"),
        (c1, stats_msg(match, late=-1), "bad_request"),
        (c1, stats_msg(match, frames=True), "bad_request"),
        (c1, stats_msg(match, their_fps=float("inf")), "bad_request"),
        (c1, stats_msg(match, depth=[1, 2, 3]), "bad_request"),
        (c1, stats_msg(match, per_second=[[60, 0, 0, 0, 0]]), "bad_request"),
        (c1, stats_msg(match, per_second=[[60, 0, 0, 0, 0, 70000]]), "bad_request"),
    ]:
        with pytest.raises(ServerError) as exc:
            await conn.request(msg)
        assert exc.value.code == code, msg
    # odd text is made plain, unknown fields dropped
    await c1.request(stats_msg(match, cpu="Intel® Core™ i5\x07", extra={"a": 1}))
    kept = json.loads(server.db.match_stats(match)[0]["stats"])
    assert kept["cpu"] == "Intel? Core? i5?" and "extra" not in kept
    # too late: hours after the match started
    server.db.conn.execute("UPDATE matches SET created = created - 4 * 3600 WHERE id = ?", (match,))
    with pytest.raises(ServerError) as exc:
        await c1.request(stats_msg(match))
    assert exc.value.code == "no_such_match"


def test_lines_a_second_at_most():
    """More than a match of 15 minutes: refused (and over P.MAX_LINE well before; the launcher halves the lines
    of a long match, "step": 2)."""
    matchstats.clean({"per_second": [[60, 0, 0, 0, 0, 0]] * matchstats.PER_SECOND_MAX})
    with pytest.raises(ValueError):
        matchstats.clean({"per_second": [[60, 0, 0, 0, 0, 0]] * (matchstats.PER_SECOND_MAX + 1)})


def test_old_lines_a_second_are_dropped(server):
    db = server.db
    db.create_match("m1", "mk9", "versus", False, None, 1, 2, 100.0)
    db.add_match_stats("m1", 1, 1, "0.5.9", {"late": 1}, [[60, 0, 0, 0, 0, 0]], now=1000.0)
    db.add_match_stats("m1", 2, 2, "0.5.9", {"late": 2}, [[60, 1, 0, 0, 0, 0]], now=5000.0)
    assert db.drop_old_seconds(2000.0) == 1
    rows = db.match_stats("m1")
    assert rows[0]["per_second"] is None and rows[0]["stats"] == '{"late":1}'
    assert rows[1]["per_second"] == "[[60,1,0,0,0,0]]"


@pytest.mark.parametrize("over,expected", [
    ({}, "smooth"),
    ({"late": 13}, "some stutter"),            # 6.5 a minute: a frame shown again every 9 s
    ({"frames": 6700}, "some stutter"),        # 55.8 fps
    ({"wait_ms": 600}, "some stutter"),
    ({"stalls": 4}, "some stutter"),
    ({"late": 61}, "stutter"),                 # 30 a minute
    ({"frames": 5900}, "stutter"),             # 49 fps
    ({"wait_ms": 3000}, "stutter"),
    ({"fight_secs": 3.0}, "no fight"),
    ({"depth": [0, 0, 500, 500]}, "smooth"),   # deep rollbacks are jumps, not stutter (shown apart)
])
def test_verdict(over, expected):
    st, _ = matchstats.clean(game_stats(**over))
    assert matchstats.verdict(st) == expected


async def test_the_match_log_shows_both_sides(server, connect, udp_peers, capsys):
    sides, match = await setup_match(server, connect, udp_peers, ["Mileena", "Baraka"], ranked=True)
    (c1, go), (c2, _) = sides[1], sides[2]
    await report(c1, match, 2, 1, 2)
    await report(c2, match, 2, 1, 2)
    await c1.request(stats_msg(match))
    db = server.cfg.db_path
    assert matchlog.main(["--db", db, "--hours", "1"]) == 0
    out = capsys.readouterr().out
    assert "1 matches since" in out and "0 with stats from both players (1 smooth on players' PCs)" in out
    assert f"{match}  versus ranked" in out and f"{go['p1_name']} vs {go['p2_name']}" in out
    assert f"{go['p2_name']} won 1-2" in out
    assert "smooth" in out and "59.9 fps" in out and "1.5 late/min (41 ms)" in out
    assert "no stats (a launcher before 0.5.9, or none sent)" in out
    # one match, second by second: the seconds where something happened
    assert matchlog.main(["--db", db, "--match", match]) == 0
    out = capsys.readouterr().out
    assert "2:29   57    3    20    5    0    0" in out
    assert "1 of 120 seconds with fight frames had something" in out
    assert "AMD Ryzen 7 9800X3D; NVIDIA GeForce RTX 4070" in out
    # another player's: none
    assert matchlog.main(["--db", db, "--player", "Nobody"]) == 0
    assert "0 matches since" in capsys.readouterr().out
    assert matchlog.main(["--db", db, "--json"]) == 0
    doc = json.loads(capsys.readouterr().out)
    [m] = doc["matches"]
    assert m["id"] == match and m["sides"]["1"]["stats"]["late"] == 3 and len(m["sides"]["1"]["per_second"]) == 150


async def test_the_match_log_page(server, connect, udp_peers, capsysbinary):
    sides, match = await setup_match(server, connect, udp_peers, ["Cyrax", "Sektor"], ranked=False)
    (c1, go), (c2, _) = sides[1], sides[2]
    await c1.request(stats_msg(match))
    await c2.request(stats_msg(match, frames=4800, late=900, wait_ms=4100.0, waits=230, cpu="<b>odd</b>"))
    assert matchlog.main(["--db", server.cfg.db_path, "--html"]) == 0
    page = capsysbinary.readouterr().out.decode("utf-8")
    assert page.startswith("<!doctype html>") and "<title>MK9 Match Log</title>" in page
    assert "1 matches since" in page and f"{go['p1_name']} vs {go['p2_name']}" in page
    assert f"{go['p1_name']}: smooth" in page and f"{go['p2_name']}: stutter" in page
    assert page.count("<svg") == 2 and "150s: 3 frames shown again" not in page and "149s: 3 frames shown again" in page
    assert "&lt;b&gt;odd&lt;/b&gt;" in page and "<b>odd</b>" not in page
