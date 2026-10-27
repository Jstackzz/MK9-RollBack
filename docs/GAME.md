# MK9 on PC: the game side

Mortal Kombat Komplete Edition (Xbox 360, title 575207FD, `default.xex` 0.0.0.8) recompiled to
native x64 code with the ReXGlue SDK (`external/rexglue-sdk`). This page covers what the game
executable (`mk9/`) adds on top of the recompiled code: netplay, the launcher link, spectating and
the tools to test them. The launcher and server are described in [LAUNCHER.md](LAUNCHER.md).

## Build and run

```
mk9\build.cmd release            # -> mk9\out\build\win-amd64-release\mk9.exe (+ rexruntime.dll, rexgpu-xenos.dll)
tools\run-mk9.cmd <logname>      # plain start, logs in mk9\out\build\win-amd64-release\logs
```

The game reads its disc files from `--game_data_root` (default `<exe>\game`; here `game\disc`, the
extracted ISO) and keeps saves in `--user_data_root` (default `<exe>\user`).

## Source map (`mk9/src/mk9`)

| file | what |
|---|---|
| `frame.cpp` | the frame boundary: FEngineLoop::Tick (`sub_823621F8`), top/end hooks, frames run again |
| `input.cpp` | XamInputGetState / GetCapabilities / GetKeystrokeEx answered from one pad source chain: autopilot override (offline) > netplay or test scripts > the PC's controllers (remapped, `controls.cpp`) |
| `game.cpp` | what netplay and the launcher read from MK9 (addresses below), the random generator agreement, per-call-site menu streams |
| `net/netplay.cpp` | UDP netplay for 2-4 slots: barriers, lockstep, rollback mode, checks, the spectator feed |
| `net/rollback.cpp`, `net/synctest.cpp` | copy-on-write guest memory snapshots and the single-PC sync test (experimental, see below) |
| `link.cpp`, `autopilot.cpp` | the launcher protocol (cmd.txt / status.txt) and the menu autopilot |
| `engine.cpp` | UE3 thread facts for rollback (render command ring, D3D device), `MK9_ONETHREAD` |
| `diag.cpp`, `snap.cpp`, `testinput.cpp`, `randtrace.cpp` | diagnostics: thread/heap lists, memory dumps, in-game PNG snapshots, scripted pads, random draw traces |

## Game state used (all in the executable's data, stable across runs)

| address | meaning |
|---|---|
| `0x833C6C58` | phase: 2 boot/title, 3 menus, 7 select screens, 1 VS screen while the fight loads, 8 fight (intros and every round), 11 match over (finisher, victory, CONTINUE?) |
| `0x833AEA80` | front-end screen: 0x18C title, 0x190 main menu (1 PLAYER), 0x194 (2 PLAYERS), 0x198 VERSUS MODE submenu, 0x250-0x284 select screens, 0x4C VS screen, 0x62 CONTINUE? |
| `0x833C77B8` | 1 while the title screen is up |
| `0x833C7A38`, `0x833C7A3C` | rounds won by player 1 / 2 (final once CONTINUE? is up) |
| `0x833CED28`, `0x833CED24` | round number, round timer |
| `0x834161D8` | the game's random generator (UE3 LCG; `sub_826D91C0` int, `sub_826D91E8` float, `sub_826D3088` seed) |

The C runtime's `rand()` is used only by FMOD (sound variations) and is not part of the simulation.

## Netplay

Each PC is one player slot (0 = P1). All games meet at barriers and from then on every slot's
input for each frame is the same everywhere. Barriers: the launcher's start (at the hold point)
and every time a fight begins (phase turns to 8). At a barrier the game's random generator is
set from the agreed seed; outside fights each call site draws from its own stream (agreed seed,
site, count), so loading-time differences between PCs cannot change menu picks (the arena
variant, Test Your Luck's slots). Every 30 frames both sides compare a state hash.

- **Lockstep** (default): a frame runs once every player's input for it is there; input delay 2.
- **Rollback** (`netcode=rollback`, experimental): frames of a fight run ahead on guessed inputs
  and a wrong guess restores guest memory and runs the frames again. The restore is not safe yet
  with UE3's other threads (the rendering thread, shared allocator pages): the single-PC sync
  test still crashes after the first restore. Do not use it for real matches yet.

Spectators: a player's game writes its barriers, inputs and checks to the launcher's local feed
(TCP, `feed=127.0.0.1:port`); a spectator's game (`watch=1 feed=...`) reads every player from it
and plays the match in lockstep without a player of its own. The launcher relays feeds through
the server (FeedLink).

## The launcher link

With `MK9_LINK=1` the launcher owns the game; see the header of `link.cpp` for the exact
`cmd.txt` / `status.txt` lines. The game boots to the hold point (main menu, fresh save) and says
`ready`; `start` meets the other games there, slot 0's autopilot steers both through the menus to
the mode's select screen (ONE VS ONE, TWO VS TWO, TEST YOUR LUCK), where each player picks with
their own controller; after the match it reports `result winner=N p1_rounds=.. p2_rounds=..`.
At the CONTINUE? menu (steered by the loser's controller, as on the console) `rematch` plays the
same fighters again, `rematch select=1` picks again, `park` quits to the main menu.

## Testing on one PC

```
. tools\netplay-local.ps1; Start-NetplayLocal -Out <dir>        # two games, scripted menus and fight
. tools\link-local.ps1;    Start-LinkLocal -Out <dir> -Spectator -Attack -Rematch -Park
. tools\synctest.ps1;      Start-Synctest -Out <dir> -Depth 1 -From 5400   # rollback restore test
python -I tools\netcmp.py a\net.log b\net.log                  # frame logs of two games side by side
python -I tools\randcmp.py a\rand.txt b\rand.txt               # MK9_TRACE_RAND random draw traces
```

## Rollback: where it stands

`net/rollback.cpp` snapshots guest memory copy-on-write per frame (pages write-protected at the
top of each frame; the first write copies the page into that frame's undo log) and
`net/synctest.cpp` checks that a frame run again writes what it wrote the first time. On MK9 the
first restore crashes the game. What was found (`tools/synctest.ps1`, `MK9_RB_TRACE_FOREIGN=1`):

1. The rendering thread runs the main thread's render commands up to a frame later; a restore
   under it made it read stale commands. Fixed: the frame top waits for it to drain
   (`engine::WaitRenderIdle`), and the command ring and the D3D device are never put back.
2. The general heap mixes objects of every thread on the same pages. A page another thread
   wrote first in a frame is "foreign" (never logged), yet the main and rendering threads keep
   writing their own objects on such pages (traced in dozens of functions: allocator internals
   around `0x82A4F700`, game code, FMOD's event system on the main thread). Those writes are not
   undone, so a restore leaves freed or half-updated objects behind (null and garbage pointers).
3. `-ONETHREAD` rendering (no rendering thread) crashes at boot: other threads queue render work.

What a working rollback needs, in order: the fight's allocations from a heap of their own that
only the simulation threads touch (MK9's MK MemoryManager, `sub_829AFB28` and its pools, routes
by thread), FMOD's memory left alone and its event starts muted while frames run again, and the
rendering thread's memory restored with the rest (it is idle at frame tops). Until then,
lockstep is the netcode to use.

## Known gaps

- Rollback restores crash (see above); lockstep is what works.
- Four-player TWO VS TWO (slots 2-3) is in the netplay core but untested.
- The autopilot steers menus by fixed steps from a fresh save's cursor; the roster grid is not
  mapped yet, so the launcher cannot pick a named fighter (players pick in the game).
- GPU emulation draws a striped texture on fighters after hits (also in Xenia).
