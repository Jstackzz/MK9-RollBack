# MK9 Online: launcher and server

The launcher (`launcher/`, Qt 6 Widgets, C++20) and the lobby/matchmaking server (`server/`, Python asyncio,
standard library only) of the native PC port of Mortal Kombat Komplete Edition (Xbox 360). Both started as copies of
the MK3D Online ones; the wire protocol is MK3D's ([mk3d-reference/PROTOCOL.md](mk3d-reference/PROTOCOL.md)) with the
MK9 changes in [PROTOCOL.md](PROTOCOL.md). The game's side of the launcher link is `mk9/src/mk9/link.cpp`.

```
launcher/
  data/mk9.json          the game, its modes and roster (compiled into the exe)
  data/sounds/           match-found.wav, the MATCH FOUND chime (compiled in; tools/make_chime.py writes it)
  src/                   MainWindow, pages, ServerClient (TLS), UdpRendezvous, GameProcess, MatchController,
                         FeedLink (spectators), Controls (controls.toml), Xiso (disc images), GamepadNav, Updater
  tools/fake_game.py     stand-in for mk9.exe that speaks the game contract below
  tools/make_chime.py    synthesizes data/sounds/match-found.wav (nothing recorded; the same file on every run)
  build.cmd              configure + build + windeployqt into build\launcher
server/                  python -m server; python -m server.client (scripted players); tests/ (pytest)
```

## Build

Nothing is downloaded: Qt 6.12.0 is in `external\qt\6.12.0\msvc2022_64`, SDL3 (3.5.0, for controllers) is the
ReXGlue SDK's copy in `external\rexglue-sdk\thirdparty\sdl3`, MSVC and CMake/Ninja come from the VS 2026 Build Tools
through `toolssenv.cmd`.

```
launcher\build.cmd
```

builds `build\launcher\MK9Launcher.exe` and copies the Qt runtime beside it (TLS through Windows' Schannel, no
OpenSSL). `QT_DIR`, `SDL3_DIR` and `BUILD_DIR` override the three folders; by hand it is
`toolssenv.cmd cmake -S launcher -B build\launcher -G Ninja -DCMAKE_BUILD_TYPE=Release
-DCMAKE_PREFIX_PATH=<qt> -DMK9_SDL3_DIR=<sdl3>`, `cmake --build build\launcher`, then `windeployqt`.

Server: Python 3.11+; tests with `python -m pytest server\tests` (pytest and pytest-asyncio); the certificate
helper uses `cryptography` or the `openssl` command.

## Where things are

Settings, log and sessions live beside the exe (`mk9-launcher.ini`, `mk9-launcher.log`, `controls.toml`,
`sessions\`, `cache\`), or in `%LOCALAPPDATA%\MK9\MK9 Online` when that folder is read-only. `defaults.ini` beside
the exe (a player package's server) fills in any setting the ini does not have.

| setting (`games/mk9/...`) | source build (default) | player package (default) |
|---|---|---|
| `exe` | `<repo>\mk9\out\build\win-amd64-release\mk9.exe` | `games\mk9\mk9.exe` |
| `data` (the extracted disc) | `<repo>\game\disc` | `games\mk9\game` |
| `save` (single player) | `<repo>\game\user` | `games\mk9\user` |
| `build_script`, `build_args` | `<repo>\mk9\build.cmd`, `release` | `games\mk9\build-game.cmd` |

The repository is found by walking up from the exe to a folder with `mk9\mk9_manifest.toml` and
`launcher\CMakeLists.txt` (`tools/repo_root` overrides it); a package is a `games\` folder beside the exe. Every game
the launcher starts gets a session folder `sessions\mk9-<yyyyMMdd-hhmmss>-<instance>` (the newest 30 are kept)
holding `cmd.txt`, `status.txt`, `game.log`, the netplay logs and, for an online game, its fresh save `save\`.

## The pages

- **Ranked / Kasual**: mode (One vs One, Two vs Two, Test Your Luck; Test Your Luck is Kasual only), FIND MATCH.
  Fighters are always picked on the game's own select screen (the start line says `p1=- p2=-`). The page says how
  many play MK9 right now, how many search (and for this page's queue), are in matches and in lobbies, and who
  searches (the server's `online` counts); a long search says when the server raised its max ping ("Max ping raised
  to N ms to find you a match"). The game boots in the background to its hold point while the search runs, and
  CANCEL SEARCH closes it again (and single player the search opened), unless a lobby's Ready wants it or it was up
  before. A match found has 20 s to be accepted: MATCH FOUND stays on top of the games (brought back in front every
  400 ms while a game window takes it), plays a chime (Settings), and shows the opponent's connection, ranked too:
  the Wi-Fi fan (amber) or the wired bars (green) and the ping, "about N ms between you" when the server knows it
  (an earlier match, or both ways to the server when the match goes through it), else their ping to the server. Then
  the launcher
  punches the opponent on the game port, measures the ping, writes the start line and reports what the game writes
  to `status.txt`. Two who cannot reach each other play through the server's UDP port, and a player whose network
  lets no UDP through at all (no answer from the server's UDP port within 3 s) through the server connection
  (PROTOCOL.md, "A match through the server connection"): the game's peer is then `TunnelBridge`, a socket of the
  launcher on 127.0.0.1. While a match runs the button is LEAVE MATCH (also in the footer): Kasual and lobby matches end
  without counting, a ranked one counts as leaving the set (it asks first). After a queue match the series card
  offers REMATCH (the same fighters), SELECT (both pick again) and LEAVE for 60 s; a ranked set (first to 2) plays
  on by itself. When both games still wait at their CONTINUE? menu the next match starts right there with the
  game's own `rematch` (no new connection), otherwise as a new match. A Kasual match that a game closed during, or
  whose games lost each other, does not count and the rematch is offered again (from new games): "Your game closed
  during the match against X: it does not count." A ranked one still loses the set. *Play single player while you wait* starts
  single player during a search (off by default); a match found freezes it and it comes back afterwards.
- **Lobby**: create (name, mode, optional password, best of 1/3/5, *King of the Hill*), join from the list or by its
  5-letter code, chat, Ready. The two to play ready (both players, or the first two in a King of the Hill line), the
  server starts the match after a 3 s countdown (STARTING IN 3, 2, 1, then MATCH IN PROGRESS); with a server before
  0.3.0 the host's START does it. The members' table has a Connection column (the Wi-Fi or wired icon, their ping to
  the server). START on a controller presses Ready, and after a lobby match the lobby comes back in front with Ready
  focused. Leaving the lobby closes its game once nothing needs it (a match it plays or watches ends first). A
  dropped connection keeps the place 60 s: the next login takes it back, and a spectator's game watches on meanwhile.
  *Watch selected* / *Watch* make you a spectator: each of the lobby's
  matches plays in your game from both players' feeds (`FeedLink`, a TCP port of the launcher on 127.0.0.1).
  King of the Hill: up to 8 players in line, the first two fight (the first as P1), the winner stays, the loser
  goes to the end; a match without a result keeps the king and sends the challenger to the end, and a player who
  leaves a match or ends it gives up their place.
- **Single player**: PLAY starts the game plainly (no `MK9_LINK`) with the persistent save folder: the game's own
  menus, every offline mode. In a source build the **Local test** card starts two games on this PC (slots 0 and 1,
  UDP 7001/7002 on 127.0.0.1, windowed) with matching start lines and the Settings' netcode; no server.
- **Leaderboard**: top 100 by Elo per ranked mode (One vs One, Two vs Two).
- **Build game**: pick the Xbox 360 disc image (`.iso`), EXTRACT AND BUILD. `src/Xiso.cpp` finds the game partition
  at 0, 0xFD90000 (XGD2: this disc), 0x2080000 (XGD3) or 0x18300000 (XGD1), checks `default.xex`'s SHA-1 against
  `xex_sha1` in `data/mk9.json` (another release asks first) and the free space, and extracts into the game data
  folder (each file as `.part`, then renamed; files already there in full are kept). Then it runs the build script
  (`cmd /s /c "<script>" <args>`) and streams its output into the page; Cancel kills the whole process tree. A
  package without its `build-game.cmd` says so.
- **Controls**: the Xbox 360 controller's buttons (up down left right a b x y lb rb lt rt start back ls rs): up to
  two keys each, and what each button of a PC controller presses (south east west north left_shoulder
  right_shoulder left_trigger right_trigger back start left_stick right_stick dpad_*). Saved at each change to
  `controls.toml` in the format `mk9/src/mk9/controls.cpp` reads (`[keyboard] enabled`, `[keys]` with the game's
  key names, `[buttons]`); every game started gets its path in `MK9_CONTROLS`. Online matches run on a fresh save,
  so they play with the game's default button layout: remap here.
- **Settings**: server address and pinned fingerprint, region, max ping, **input delay** (Auto, the default: from
  the ping measured when the match connects; or 0-6 frames), **netcode** (Rollback,
  the default, or Lockstep: a match plays rollback only when both players chose it), game UDP port,
  *Play a sound when a match is found* (on), UPnP, *Hide my IP address from opponents* (off: every match of the
  player's goes through the server, `relay` in `queue_join` / `lobby_ready`, at the cost of ping for players far
  from it), **display** (below), the game's paths (table above), Python and local test ports.

  Display, all on the game's command line when it starts (the ReXGlue SDK's cvars; a game too old for one sets it
  aside): the game window (Fullscreen, borderless at the monitor's own resolution, or a Window: `--fullscreen` /
  `--no-fullscreen`), the window's size (1280x720, the game's own, or a 16:9 size that fits the main monitor:
  `--window_width` / `--window_height`, divided by the display's scaling, which the SDK multiplies them by), the
  **resolution** the game draws at (1280x720 as on the Xbox 360, or 2x / 3x: `--resolution_scale`) and **V-sync**
  (On, the default: `--no-d3d12_allow_variable_refresh_rate_and_tearing`, a swap chain that never tears, every
  refresh showing the newest frame whole; Off: the SDK's own presenting, each frame at once with tearing allowed,
  which a G-SYNC or FreeSync monitor follows and any other tears across). The SDK presents with sync interval 0 either
  way: the game keeps its own 60 frames a second and never waits for the display.

A controller steers the launcher as in MK3D (`GamepadNav`, SDL3): d-pad or stick moves the gold focus, A presses, B
backs out of dialogs (never MATCH FOUND or the rematch prompt), LB/RB switch pages, START finds a match (in a lobby:
Ready). While a match is set up, played or watched the buttons are the game's alone (a launcher left in front must
not act on a fight's presses), and Sign out waits for the match's end.

What the launcher shows and logs never names an address: not the opponent's (the start line's `peers=` is logged as
`(hidden)`, the punch says only "directly" or "through the server"), not this PC's public or home address (the UDP
hello, the UPnP status). The server sends the home network's address only to a player behind the same router.

A match that went out of step (`desync` from the game) sends a desync report a moment later (`desync_report`,
PROTOCOL.md; servers from 0.3.0): this PC's Windows and processor, the game's `mk9 net:` / `mk9 link:` lines from the
match's start (addresses as `x.x.x.x`, the user folder as `*`), the match's last frames from `net-<match>.log`, and the
extracted game's files in brief (count, bytes, a hash of names and sizes); the server logs both players' side by side.
A game that ends during a match reports `left` with how (`crash C0000005`, `abort`, `closed`, `exit code N`).

## Crash reports

A game the launcher started that ends by itself with an exit code other than 0 (not asked to quit, not killed) gets
a report in `crashes\<yyyyMMdd-hhmmss>-mk9\` beside the launcher's log (`CrashReports`; the newest 20 are kept):

- `report.txt`: the time, the launcher's and the game's versions, the exit code in words, this PC (Windows,
  processor, memory, the graphics card the game's log names), the launcher's last match messages, the game's own
  report (`crash.txt`, mk9/src/mk9/crash.h: the exception and where, as `sub_82xxxxxx` for recompiled code, both call
  stacks, the guest registers, the game's state and modules), then the last 200 lines of the game's log and 60 of the
  launcher's;
- `crash.dmp`, the game's minidump, moved from its session, and `game.log`, its whole log.

The launcher then shows THE GAME CRASHED: SEND REPORT (`crash_report`, PROTOCOL.md; only `report.txt`, with the
user folder's path in it replaced by `%USERPROFILE%`; `sent.txt` in the folder records it), OPEN FOLDER, CLOSE. The
server keeps what it gets in `crashes\` beside its database (`/var/lib/mk9/crashes` on the official one). The
stand-in game (`.py`) gets no reports.

## The game contract

The launcher owns a game it starts for online play. Environment: `MK9_LINK=1`, `MK9_CMD=<session>\cmd.txt`,
`MK9_STATUS=<session>\status.txt`, `MK9_SESSION_DIR`, `MK9_INSTANCE` (1, or 1/2 in a local test),
`MK9_NET_LOG=<session>\net-{match}.log` (unless already set), `MK9_CONTROLS=<controls.toml>`. Command line:
`--game_data_root=<disc folder> --user_data_root=<session>\save --cache_root=<data dir>\cache\mk9-online-<instance>
--log_file=<session>\game.log --fullscreen|--no-fullscreen` (the shader cache outlives the fresh saves; one per
instance). Single player: no `MK9_LINK`/`MK9_CMD`/`MK9_STATUS`, `--user_data_root=<save folder>`, no
`--cache_root`. The file rules are MK3D's (PROTOCOL.md 3.2: atomic replace, the command claimed by renaming it to
`cmd.txt.taken`, one command at a time, a newer one replacing one not taken within 2 s).

Commands (`cmd.txt`):

```
start mode=versus|tag|tyl slot=<0|1> players=2 local_port=<udp> peers=<other slot>@<ip:port> delay=<frames>
      netcode=lockstep|rollback seed=<u32> match=<id> p1=- p2=- rounds=<1|3|5> ranked=<0|1> p1_name=<a> p2_name=<b>
      [feed=127.0.0.1:<port>]                                   a lobby match: its spectators' feed
start mode=<mode> watch=1 feed=127.0.0.1:<port> players=2 netcode=<...> seed=<u32> match=<id> p1=- p2=-
      rounds=<n> ranked=0 p1_name=<a> p2_name=<b>               a spectator
rematch match=<id> seed=<u32> [select=1]                         the series' next match from CONTINUE?
park | quit
```

Side 1 of the server is slot 0 (P1, left), side 2 slot 1. `delay` is `match_go`'s: the larger of both players'
settings, an Auto one being clamp(ceil((rtt_ms / 2 + 8) / 16.67), 2, 8) frames from the match's measured round trip
(the server computes it, so both games get the same; at least 1 in ranked; a local test's Auto is 2). `netcode` is
the server's decision; `rounds`, `ranked` and the names are informational. A start goes
only to a game whose status is `ready`; a `rematch` only to one whose status is `result`.

Status (`status.txt`, first line): `booting`, `ready`, `in_match match=<id>`, `result winner=<1|2> p1_rounds=<n>
p2_rounds=<n> match=<id>`, `desync frame=<n> match=<id>`, `disconnected match=<id>`, `error <text>`, `quit`. The
launcher reports `result`, `desync`, `disconnected` and `error` to the server, a game that exits during a match as
`left`. After the server's `match_result`:

- a queue or series match with an agreed result: the game stays at its CONTINUE? menu; the series' next match is
  `rematch` there when both launchers said so (`series_ready` with `continue`), else it is parked (`park`, back to
  `ready`) for a new start;
- a lobby match, or one without an agreed result at CONTINUE?: `park` (30 s to come back to `ready`);
- a match that ended anywhere else (left in the middle, the games lost each other): the game is started again (a
  new session, a fresh save), since a park from there cannot bring it back.

A start the game turns down (`error ...`, then `ready` again) is noticed within 5 s; a watched match whose game
turns it down or never connects to its feed within 10 s is dropped with a notice, and the game is started again.

## Testing without the game

`launcher/tools/fake_game.py` speaks the contract above (set Settings, Game, Executable to it: `.py` runs with the
configured Python). It boots to `ready`, binds `local_port`, exchanges heartbeats with the peers, writes `in_match`,
and after `MK9_FAKE_SECONDS` (6) a result derived from the seed (the rule of `server/client.py`), then waits at its
"CONTINUE?" for `rematch` (same peers, new id and seed) or `park`; a park from the middle of a match leaves it lost
(`booting`), like the game. With `feed=` it writes MK9N feed packets; with `watch=1` it reads the feed and reports the
seed's result. `MK9_FAKE_RESULT=desync|none`, `MK9_FAKE_BOOT`, `MK9_FAKE_PARK` vary it; with `desync` its log has
a match's `mk9 net:` lines and its `net-<match>.log` the last frames, for the desync report; `MK9_FAKE_EXIT=<code>` ends
it by itself with that exit code halfway through a match.

What was run for this version (2026-10-27), all on one PC with Qt's offscreen platform
(`QT_QPA_PLATFORM=offscreen`, `QT_QPA_PLATFORM_PLUGIN_PATH=<qt>\plugins\platforms`, since windeployqt ships only
`qwindows`):

- `MK9Launcher.exe --config <ini> --local-test [--mode tag]` with the stand-in: both games ready, the two start lines,
  `in_match`, agreeing results, park, `ready`.
- A server (`python -m server --port 7787 --udp-port 7788` with a `server.makecert` certificate) and two launchers
  `--user <a|b> --register --queue casual --auto-accept 2 --rematches 2 --rematch-choice rematch,select`: match 1 set
  up (punch, `match_go`, start), then `rematch` (same fighters) and `rematch select=1` from CONTINUE?, all results
  agreed, the series timing out and both games parked back to `ready`.
- Three launchers `--lobby host|join|watch --lobby-name ...`: the lobby match with `feed=`, the spectator's
  `watch=1` start line, its result, park; the server passed the players' MK9N feed on. And `--lobby host --koth`.
- `python -m server.client demo --port 7787 --fingerprint <fp>`: two scripted players, one agreed match.
- `--extract <the Komplete Edition iso>` with a stand-in build script: XGD2 at 0xFD90000, 1265 files, 7.27 GB,
  `default.xex` SHA-1 as in `mk9.json`, every file byte-identical to `game\disc`, the script's output streamed.
- `python -m pytest server\tests`: 88 passed.

And for 0.4.0 (2026-10-28), the same way:

- Two launchers and the stand-in against a local server, one with `[dev] block_udp=true` (every UDP datagram it
  sends goes nowhere): after 3 s without a `hello_ack` it asked for a tunnel, punched and pinged through the
  connection, its game's peer was `TunnelBridge`, and two matches (the series went on, set up anew) agreed with
  the other launcher on the server's UDP relay, 73 packets each way, none dropped. Both blocked: the same through
  two connections.
- A launcher whose real game crashed on purpose (`MK9_CRASH_TEST=600`) while it searched: `crashes\<time>-mk9\`
  with `report.txt` (26 KB), `crash.dmp` (1.2 MB) and `game.log`; `--send-crash-reports` sent it, and the server's
  copy had no user folder path left in it.
- `python -m pytest server\tests`: 95 passed (tunnels and crash reports: `test_tunnel.py`).

And for the fixes and features from MK3D Online (2026-10-29), with the stand-in and `QT_QPA_PLATFORM=offscreen`:

- A local server and two launchers, Kasual, one hiding its address, both games desyncing: MATCH FOUND's connection
  ("ping about 87 ms between you": both ways to the server), the match through the relay, both desync reports in the
  server's log (18 lines each, the stand-in's address written as `x.x.x.x`), the chime played; the launchers' logs had
  no address but the server's and their own bound port.
- A lobby with a scripted spectator: its states had the members' `ping` and `starts_in` 3.0, the countdown started
  the match ("started (both ready)"), both left (`--leave-lobby`) and their games closed.
- A game that ended itself mid-match (`MK9_FAKE_EXIT`, 0xC0000005): `left` with `crash C0000005`, the Kasual series
  went on; a third launcher's search cancelled after 8 s (`--cancel-search`) closed the game it had started.
- These launchers against the deployed 0.2.0 server's code (the host's START, no connection, desync reports not
  sent), and the 0.4.0 launcher against the new server (Kasual, and the lobby: its host's START came first).
- `python -m pytest server\tests`: 113 passed.

Command-line options (development and automation) are MK3D's (`--config`, `--server`, `--fingerprint`, `--user`
with `MK9_DEV_PASSWORD`, `--register`, `--queue casual|ranked`, `--mode versus|tag|tyl`, `--auto-accept N`,
`--rematches N`, `--rematch-choice`, `--leave-series`, `--solo`, `--lobby host|join|watch`, `--lobby-name`,
`--lobby-matches`, `--page`, `--local-test`, `--extract`, `--auto-update`, `--updated`, `--cancel-search N`,
`--leave-lobby`) plus `--koth`, `--netcode lockstep|rollback` and `--solo-wait on|off` (saved); `--practice` and
`--death-traps` are gone.

## Not done yet

- **Fighters in the launcher**: every MK9 mode picks on the game's select screen. The roster ids of `mk9.json`
  (`CyberSubZero`, `NoobSaibot`, `SonyaBlade`, ...) are what a `p1`/`p2` would name, but the game's autopilot does
  not map them yet (the disc's asset names differ: `CyberZero`, `Noob`, `Sonya`), so the launcher always sends `-`.
  Kratos is PlayStation-only and not on the Xbox 360 disc; Goro, Kintaro and Shao Kahn are listed as bosses.
- **The player package**: `tools\package.ps1` / `tools\release.ps1` (see GAME.md). Its `defaults.ini` (the server
  the launcher uses until a player picks another) comes from `dist\defaults.ini`, which is kept out of the
  repository because it names the machine: `[server]` `address=<host>:<port>` and `fingerprint=<SHA-256>`. A
  launcher that had saved the old built-in server (`127.0.0.1:7777`) takes the package's once.
- **Two vs two with four people**: the game takes `players=4`; the server and launcher pair two players only.
- **Rollback**: the default; tested on one PC with simulated lag (see GAME.md), not yet between two PCs over the
  internet. Lockstep stays in Settings.
- **Display**: only fullscreen or a window; the resolution and v-sync settings of MK3D are gone (the game's own
  `mk9.toml` cvars apply).
- The launcher was not run against the real `mk9.exe` here (it was being rebuilt); everything above used the
  stand-in, whose behaviour follows `mk9/src/mk9/link.cpp` as of 2026-10-27.
- **Hosting**: `server/deploy/install.sh` installs `mk9-server` (`/opt/mk9`, `/var/lib/mk9`); `MK9_TCP_PORT` /
  `MK9_UDP_PORT` pick other ports than 7777/7778 on a machine that already runs the MK3D server. The official
  server runs that way beside MK3D's on the same Oracle machine, on TCP 7787 and UDP 7788 (installed with
  `sudo env MK9_TCP_PORT=7787 MK9_UDP_PORT=7788 sh server/deploy/install.sh <address> <oldest launcher>`; MK3D's
  service, folders and firewall rules are not touched). Oracle's security list must allow both ports too. The
  MK3D `deploy-server.ps1` / `host-server.ps1` have no MK9 versions yet.
