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
| `net/rollback.cpp`, `net/synctest.cpp` | copy-on-write guest memory snapshots and the single-PC sync test (see "Rollback" below) |
| `simheap.cpp`, `gmalloc.cpp` | the simulation heap and the audio heap, and GMalloc's routing to them |
| `link.cpp`, `autopilot.cpp` | the launcher protocol (cmd.txt / status.txt) and the menu autopilot |
| `engine.cpp` | UE3's other simulation threads for rollback (rendering thread, job workers, PhysX, the thread pool): who they are, holding them still at a frame's top, what of theirs a restore leaves alone; `MK9_ONETHREAD` |
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
variant, Test Your Luck's slots; in fights every MK script process has a stream of its own, so a
script drawing more often, an arena's ambience reacting to a sound ending, say, leaves the
fighters' numbers alone). Every 30 frames both sides compare a state hash: the round timer, the
round, the rounds won and the draws made at every call site so far (a fight that goes differently
draws differently).

- **Lockstep** (default): a frame runs once every player's input for it is there; input delay 2.
- **Rollback** (`netcode=rollback`): frames of a fight run ahead on guessed inputs (up to 8) and a
  wrong guess puts the game back to the top of that frame and runs the frames since again with
  the real inputs (see "Rollback" below). Menus, loading and the match's end run in lockstep.

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
. tools\synctest.ps1;      Start-Synctest -Out <dir> -Depth 4 -From 5400   # rollback restore test
. tools\netplay-local.ps1; Start-NetplayLocal -Out <dir> -Mode rollback -Delay 1 -Lag 50 -Jitter 30 -AttackB -Sites
python -I tools\netcmp.py a\net.log b\net.log                  # frame logs of two games side by side
python -I tools\randcmp.py a\rand.txt b\rand.txt               # MK9_TRACE_RAND random draw traces
```

## The player package (each PC builds its own game)

Nobody ships the game's code: like the Tekken 6 and MK3D projects, every player translates their
own disc. `tools\package.ps1` (`New-PlayerPackage`) assembles `dist\MK9Online`:

| part | from |
|---|---|
| `MK9Launcher.exe` + Qt | `build\launcher` (`launcher\build.cmd`) |
| `toolchain\` | clang, lld-link and clang's headers out of `external\llvm` (~230 MB) |
| `games\mk9\sdk\` | the ReXGlue runtime and codegen tool, built standalone and installed: `cmake --build external\rexglue-sdk\out\build\win-amd64` then `cmake --install ... --prefix dist\sdk` |
| `games\mk9\project\` | the game's CMake project, `src`, `config`, a manifest pointing at `games\mk9\game` |
| `games\mk9\build-game.cmd` | `tools\player\build-game.cmd` |

On the player's PC the launcher extracts the disc into `games\mk9\game` and runs
`build-game.cmd`: codegen on their `default.xex`, then CMake + Ninja + clang against the prebuilt
runtime (Visual Studio Build Tools with "Desktop development with C++" must be installed for the
MSVC libraries and the Windows SDK). Tested here: the packaged build took about 12 minutes and
the resulting `mk9.exe` boots to the hold point.

## Rollback

`net/rollback.cpp` keeps guest memory copy-on-write: at the top of each frame every tracked page
is write-protected; the first write to a page in a frame copies it into that frame's undo log, and
going back to frame r plays the logs since r backwards. MK9 is UE3 on six kinds of threads, and a
restore is only right if the memory it puts back is the simulation's and nothing else is running:

1. **What the simulation is.** The main thread, the rendering thread, MK's three job workers,
   PhysX's threads (it simulates its scenes on its own threads, and an async scene's step runs
   across a frame's top) and UE3's thread pool. `engine::Hold` holds all of them still at every
   frame's top while memory is kept or put back: the rendering thread with its ring drained, asleep
   in its idle loop; the workers in their 2 ms idle wait; PhysX's threads in `Event::wait` or
   between tasks; the pool's threads in their DoWorkEvent wait with no work given. PhysX's events
   (start, done) are kept with every frame and set back with it (`SaveEvents`), so a step that was
   running across a frame's top runs again.
2. **Whose memory.** `GMalloc` (the MemoryManager at `0x83423BA8`) is routed from boot on
   (`gmalloc.cpp`): the simulation's threads allocate from the simulation heap (a TLSF heap in a
   region of its own, its bookkeeping inside it), FMOD (from its code or UE3's file callbacks for
   it) from the audio heap, which rollback never tracks, and every other thread from the engine
   heap. While armed the simulation never writes the engine heap's bookkeeping: its frees of
   engine blocks wait for disarm, unless a rollback took the frame back. A simulation block
   another thread writes (FMOD's file handles, a loader's buffer) is that thread's from then on.
3. **What a restore leaves alone** (volatile bytes): the rendering command ring and its control
   block, the D3D device, every critical section outside the simulation heap (a lock's state never
   goes back), the GPU callback's frame-time globals and the engine heap's own state. Pages other
   threads write first are "foreign" (never put back), but a page holding volatile bytes is logged
   whoever writes it.
4. **The host.** `ReadFile` into a write-protected guest page fails in the kernel instead of
   faulting: the SDK's file read takes such pages first (`patches/`).

The sync test (`MK9_SYNCTEST=4`) goes back 4 frames every 10 frames through a scripted fight: 420
rollbacks and 1,680 frames run again without a crash, worst rollback about 70 ms (frames run
again render too). `MK9_SYNCTEST_VERIFY=1` compares every tracked page after the first restore
with a copy taken at that frame's top (only foreign audio pages differ) and lists what a frame
run again wrote differently. Its own pass/fail ("frames differed") counts every byte, D3D fences
included, so it always reports differences; the two-game test is the real check: with 50 ms lag,
30 ms jitter and both players attacking, two games roll back about a hundred times each over a
2-minute match at 60 frames/s and their checks agree (one check in 322 differed once, a script
drawing a frame later).

Diagnostics: `MK9_RB_TRACE_FOREIGN=1` (who writes which pages first, stacks of foreign writers),
`MK9_GMALLOC_TRACE=1` (frees traced, double frees explained), `MK9_NET_SITES_LOG` (draws per
call site at every check; `-Sites` in `netplay-local.ps1`).

## Known gaps

- Rollback is tested on one PC over loopback with simulated lag (see "Rollback"), not yet over the
  internet. Frames run again also render; a long rollback costs about 17 ms a frame.
- Sounds started in frames a rollback takes back keep playing (FMOD is not rolled back).
- Four-player TWO VS TWO (slots 2-3) is in the netplay core but untested.
- The autopilot steers menus by fixed steps from a fresh save's cursor; the roster grid is not
  mapped yet, so the launcher cannot pick a named fighter (players pick in the game).
- GPU emulation draws a striped texture on fighters after hits (also in Xenia).
