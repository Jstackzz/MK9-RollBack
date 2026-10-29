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

## A match through the server connection (a tunnel)

A player whose network lets no UDP through (or lets it out and never back) plays through their TLS connection
instead. A launcher's `hello` says it can (`"tunnel": true`, launchers from 0.4.0), and a server that carries matches
that way says so in `welcome` (`"tunnel": true`, unless it runs with `--no-relay`).

- After `match_found` a launcher that has no `hello_ack` within 3 s (1 s when its hellos while searching went
  unanswered too) asks for one:

  ```json
  C: {"type": "tunnel", "match": "5f2c9a01d4e3"}
  ```

  The server marks that player. Once both accepted, the match goes to the relay at once (no direct punch), and the
  player who asked gets `"tunnel": true` in its `match_peer` (`"relay": true` as for any relayed match; the other
  player's says `"tunnel": false` when it has UDP). A `tunnel` that comes while the two punch each other directly
  starts the relay right away when both launchers send keys with their reports (below), else once both reported.
- No UDP heard from a player by the 10 s after both accepted: a player whose launcher can play through its connection
  does so, instead of the match being cancelled (`no_udp` for one that cannot).
- The relay failed for a player (UDP that gets out and never back, a router that drops it): the server tries once
  more with every player whose launcher can play through its connection doing so, before `punch_failed`.
- Datagrams go both ways as `{"type": "tun", "d": ["<base64>", ...]}`, no `id`: one or more each (from a launcher at
  most 64, each at most 2,000 base64 characters, the line under 16 KB; the server's take at most 32 KB of base64).
  The server passes them on byte for byte, as the relay does, at the relay's rate: to the other player's UDP endpoint
  (the one their hellos came from) or into their connection. The punch and the pings go this way, then the games'
  packets; `ping_report` says `"via": "tunnel"` and names the server's UDP endpoint as `remote`.
- The game knows nothing of it: it binds its port as usual, and its start line names as its peer a socket of the
  launcher on 127.0.0.1, which passes what the game sends into the connection and what comes from it to the game.
- Each relay attempt (the first relay, one with tunnels) gets a `key` of its own in its `match_peer`. `ping_report`
  sends the attempt's key back (launchers from 0.4.0), and a report with another key, one of an attempt before, is
  left aside.

## Crash reports

A launcher whose player chose to send a crashed game's report (docs/LAUNCHER.md, "Crash reports"), signed in or not:

```json
C: {"type": "crash_report", "report": "0123456789abcdef", "part": 0, "parts": 3, "data": "<base64>"}
S: {"type": "crash_report_ack", "report": "0123456789abcdef"}
```

`data` is a part of the report's text packed as Qt's `qCompress` (its size, 4 bytes big-endian, then zlib), in base64,
cut in parts of at most 12,000 characters (the launcher sends 9,000). Once every part came the server keeps the text
in `crashes\` beside its database as `<yyyyMMdd-hhmmss>-<account>-<id>.txt`, after a line saying when it came, from
which account and launcher version, and answers `crash_report_ack`. Two reports may be on their way on a connection
at once (`busy`), six an hour from one address (`rate_limited`), each at most 2 MB unpacked (`bad_request`); the
newest 500 are kept.

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

## Connections and hidden addresses (servers from 0.3.0)

Every field here is optional both ways: a launcher up to 0.4.0 sends none of them and ignores what it gets, and a
launcher after 0.4.0 works with a 0.2.0 server, which ignores them and sends none.

- `queue_join`, `lobby_create`, `lobby_join` and `lobby_ready` may carry this PC's connection: `"link": "ethernet" |
  "wifi" | "other"` (from the network interface it reaches the server through; anything else counts as unknown) and
  `"server_rtt"` (ms, 0-5000: its last round trip to the server, the UDP hello's or the TLS `ping`'s). A `lobby_ready`
  without them keeps what the member said before.
- The UDP `hello` may carry `"rtt"` (ms, 0-5000), the launcher's last round trip to that port: it updates a running
  search's `server_rtt`.
- `match_found` (queue matches, ranked too, without who it is) carries the other player's connection:

  ```json
  "connection": {"link": "wifi", "ping": 45, "estimate": 75, "measured": false}
  ```

  `link` is `null` when unknown, `ping` their `server_rtt` (`null`), `estimate` the round trip between the two when
  known (`null`): measured by an earlier match of theirs (`measured: true`), or both `server_rtt`s added when the
  match goes through the server.
- A lobby state's `members` carry `"link"` (`null` when unknown) and `"ping"` (`server_rtt`, `null`).
- `queue_join` and `lobby_ready` may carry `"relay": true`: the player hides their address (Settings, *Hide my IP
  address from opponents*). A match where either player does goes through the server from the start, never
  directly: `match_peer` with `"relay": true` and an empty `peer` for both, as with `--relay-only` (which does it for
  every match; `--no-relay` does not stop it). It combines with the tunnel: a tunnelled player's end is their
  connection, the other's the relay. A series keeps each player's choice; a relayed match is never the start of an
  in-game rematch (its games are not together).
- Matchmaking estimates a pair whose match would go through the server (one hides their address, or `--relay-only`)
  from both `server_rtt`s, and does not pair them over the tighter max ping.
- `match_peer`'s `peer.lan` (the home network's address) is sent only to a player behind the same router
  (`same_nat`); otherwise it is `""`.

## Matchmaking

- `accept_timeout` is 20 s (was 15).
- A search that finds nobody raises its max ping by itself (not a strict one, not "any"): `queued` then carries the
  rule, `"ping_widen": {"after": 30.0, "every": 15.0, "step": 25, "cap": 250}`: after `after` seconds the max ping is
  the player's own plus `step` ms, `step` more every `every` seconds, at most `cap` (never below the player's own;
  `server/matchmaking.py`, `max_ping_now`). A match found takes the raised limit (`ping_too_high` checks it).
- `online` carries each game's players, each counted once:

  ```json
  "games": {"mk9": {"playing": 6, "searching": 2, "matches": 2, "lobbies": 2}},
  "searchers": [{"name": "Kitana", "game": "mk9", "queue": "mk9/versus/casual"}]
  ```

  `searching` in a queue, `matches` in a match or a series (between two matches too), `lobbies` in a lobby
  (spectators too), `playing` any of those; `searchers` the 40 longest waiting, the longest first.

## Kasual rematches without end

A Kasual match that a game closed during (status `abandoned`) or whose games lost each other no longer ends the
series: `series` goes back to `between` and the rematch is offered again (from new games: the next match is set up
as a new one, never from CONTINUE?), however often that happens; `series_max_no_contest` ends only a ranked set. A
Kasual series ends when a player leaves it (`series_ready` with `ready: false`, a search, a lobby) or lets the offer
run out (`left`, `timeout`); a ranked set still ends `forfeit` or `void` as before. Servers before 0.3.0 ended a Kasual
series `left` or `disconnected` there.

`result` with `outcome: "left"` may say how the reporter's game ended: `"exit"` (printable ASCII, at most 40
characters: `crash C0000005`, `abort`, `closed`, `exit code N`) and `"exit_code"` (an int). The server logs them with
the match's `ABANDONED` line; anything else there is dropped, never refused.

## Lobbies: the countdown, places kept, names

- The two to play a lobby's match ready (both players, or the first two in a King of the Hill line) and no match:
  it starts by itself after a countdown (`lobby_countdown`, 3 s). A lobby's state carries `"starts_in"`: `null`, or
  the seconds left. Anything that clears a ready or takes a player away calls it off. `lobby_start` (the host) still
  starts at once; launchers after 0.4.0 never send it to a server that counts down, and know one by `starts_in`.
- A player whose connection drops (not `lobby_leave`, not `logout`) keeps their place for `lobby_rejoin_grace` (60 s):
  their next login (password or token) takes them back in, to play if a place is free (at the end of a King of the
  Hill line), else to watch; the lobby's state is pushed to them (a `lobby` without `id`), and a spectator gets the
  running match again (`spectate`, the feed from its start).
- A lobby named `<host>'s lobby` (the launcher's default) is named after the new host when its host leaves.

## Desync reports

A launcher whose match went out of step sends what its game logged of it:

```json
C: {"type": "desync_report", "game": "mk9", "match": "7658cd7bfcd7", "frame": 600,
    "lines": ["PC: Windows 11 Version 25H2, 16 threads", "CPU: ...", "[...] mk9 net: DESYNC at frame 600 ..."]}
S: {"type": "desync_ack"}
```

At most 40 `lines`, each printable ASCII and at most 200 characters (`bad_request` otherwise), 10 reports a session;
signed in only. Nothing is kept: the server logs `DESYNC_REPORT <name> <game> (launcher <version>) match <id> at frame
<n>: <k> lines`, then each line, so both players' reports sit side by side in its log. The launcher sends this PC's
Windows and processor, the game's `mk9 net:` / `mk9 link:` lines from the match's start (game.log; addresses as
`x.x.x.x`, the user folder as `*`), the match's last frames from `net-<match>.log` and a summary of the extracted
game's files (count, bytes, a hash of their names and sizes).

## Other

- `match_go` and the series' state are unchanged otherwise; `match_result` statuses are MK3D's.
- Server names: `mk9-server` (welcome), logger `mk9.server`, systemd unit `mk9-server`, client name `mk9-launcher`,
  test client `mk9-testclient` (`python -m server.client ... --game mk9 --mode versus`, password in
  `MK9_PASSWORD`).
- `RELEASES_PAGE` (where a launcher too old is sent when no update file names a release) is empty until the project
  has a releases page; a release in the update file without a `page` links to its zip.
