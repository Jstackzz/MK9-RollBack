# MK9 Online protocol: what differs from MK3D's

The server (`server/`) and launcher (`launcher/`) speak MK3D Online's protocol v1,
[mk3d-reference/PROTOCOL.md](mk3d-reference/PROTOCOL.md) (TLS NDJSON with a pinned certificate, the UDP rendezvous,
punching and relay, queues, series, lobbies, spectators), with the changes below. `PROTO` stays 1; an MK9 server
knows only the game `mk9`, so an MK3D launcher meets `bad_game`. The launcher <-> game contract (section 3) is MK9's
own: [LAUNCHER.md](LAUNCHER.md), "The game contract", and `mk9/src/mk9/link.cpp`.

## Games and modes

| game id | title | online modes | ranked | offline |
|---|---|---|---|---|
| `mk9` | Mortal Kombat Komplete Edition (Xbox 360) | `versus`, `tag`, `tyl` | `versus`, `tag` | `practice` |

`welcome` carries `"games": {"mk9": ["versus", "tag", "tyl"]}`, `"ranked": {"mk9": ["versus", "tag"]}` and
`"netcodes": ["lockstep", "rollback"]`. `queue_join` with `ranked: true` and `mode: "tyl"`, and `leaderboard` for
`tyl`, get `bad_mode` (Test Your Luck is Kasual only). Fighters are picked on the game's select screen:
`character` is `""` and `match_go`'s `p1`/`p2` are `"-"`. Arenas have no death traps: `death_traps` is accepted and
ignored by MK9 launchers.

## Netcode

`queue_join` and `lobby_ready` may carry `"netcode": "lockstep" | "rollback"` (default `lockstep`; another value is
`bad_request`). The server plays a match with `rollback` only when both players asked for it, and says so in
`match_go` (`"netcode"`) and in a spectator's `spectate`. A series keeps each player's choice.

## Input delay

`queue_join`'s and `lobby_ready`'s `delay` is 0-6 frames (MK3D: 0-4) or `"auto"`. `match_go`'s `delay` is the
larger of the two players' delays, where an `"auto"` one is `clamp(ceil((rtt_ms / 2 + 8) / 16.67), 2, 8)` with the
match's `rtt_ms` (the larger of the two players' measured round trips, `protocol.auto_delay`), at least 1 in ranked;
`"delay_auto": true` says an auto delay took part. Both games get the same value, and a series' in-game rematch the
same as its first match.

## Ending a match without a result

`result` takes one more outcome, `{"type": "result", "match": "...", "outcome": "ended"}`: the player ended the match
in the launcher (LEAVE MATCH) before the game wrote a result. Like `left` it ends the match for both at once; unlike
`left` it is nobody's fault: status `no_contest`, no win, loss or Elo change, and the series goes on (what the other
game reports afterwards, having lost this one, changes nothing). In a King of the Hill lobby the player who ended it
gives up their place in line. Launchers send `ended` for Kasual and lobby matches and `left` for ranked ones (leaving
a ranked match is leaving the set).

## The series' next match from the games' CONTINUE? menu

After a match with an agreed result the MK9 game waits at its CONTINUE? menu, still connected to the other game. A
launcher whose game is there says so when it is ready for the series' next match:

```json
C: {"type": "series_ready", "series": "8d01c3e2a9f4", "ready": true, "choice": "rematch", "continue": true}
```

When both said `continue`, the last match went straight between the two PCs (not through the relay) and its result
was agreed, the server starts the next match there, with no `match_found`, no UDP hello and no punch:

```json
S: {"type": "match_go", "match": "41b0c2d9e7aa", "rematch": "same", "series": "8d01c3e2a9f4", "side": 1,
    "remote": "203.0.113.7:7001", "rtt_ms": 38, "delay": 2, "netcode": "lockstep", "seed": 2718281828, ...}
```

`rematch` is `same` (casual, both chose REMATCH: the same fighters) or `select` (casual, either chose SELECT, and
every ranked set's next match: both pick again). The launcher writes `rematch match=<id> seed=<seed>` (plus
`select=1`) to its game instead of a start line. Otherwise the next match is set up as in MK3D (`match_found` with
`accept_required: false` and `series`, then hello, punch, `match_go`), and a launcher whose game still waits at
CONTINUE? parks it first.

## King of the Hill lobbies

`lobby_create` may carry `"koth": true`: up to 8 players (`max_players`, server option `koth_max_players`) besides
the spectators. Lobby summaries and states carry `koth` and `max_players`; a lobby's state also `king` (the name
holding the hill, `null` until a win) and `streak` (the king's wins in a row). The players' order in `members` is
the line: the first two fight (the first as side 1, P1) when the host starts, and need to be ready (`not_ready`
otherwise); the others wait. After the match the winner moves to the front (a new king starts at 1, the king adds
one) and the loser to the end; a match without an agreed result keeps the king and sends the challenger to the end;
a player who left or ended the match, or failed to show up for it, goes to the end (a king who does gives up the
hill: `king` `null`, `streak` 0), as does a king who leaves the lobby. Players may join the line, or take a place
from watching (`lobby_role`), while a match runs.

## Spectator feed

The packets in `feed` / `spectate_feed` are the MK9 netplay's (`mk9/src/mk9/net/netplay.cpp`), each after its
little-endian u16 length: header u32 magic `"MK9N"` (0x4E394B4D, little-endian), u8 type, u8 slot (the sender's: 0
is side 1), u16 match (barrier number), then 6 MEET (its READY at a barrier), 7 OWN (its own inputs), 8 CHECK, 9 END.
The server takes a player's own slot's packets of these types only (`bad_request` otherwise).

## Other

- `match_go` and the series' state are unchanged otherwise; `match_result` statuses are MK3D's.
- Server names: `mk9-server` (welcome), logger `mk9.server`, systemd unit `mk9-server`, client name `mk9-launcher`,
  test client `mk9-testclient` (`python -m server.client ... --game mk9 --mode versus`, password in
  `MK9_PASSWORD`).
- `RELEASES_PAGE` (where a launcher too old is sent when no update file names a release) is empty until the project
  has a releases page; a release in the update file without a `page` links to its zip.
