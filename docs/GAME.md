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

The SDK is ReXGlue v0.10.0 in `external\rexglue-sdk` with this project's changes on top
(`patches\rexglue-sdk-v0.10.0.patch`: file reads into write-protected guest pages, the critical
section hook rollback uses, bounded stack walks in two crash reports, no resolve copying in what draws
dropped while their pipelines are created left out: see "Blood and damage textures"; ImGui dialogs that
ask for no continuous repaint: see "Names over the fight"). On a fresh
checkout of the SDK: `git -C external\rexglue-sdk apply ..\..\patches\rexglue-sdk-v0.10.0.patch`; the
patch is `git -C external\rexglue-sdk diff --ignore-submodules`. A change to the SDK's GPU code alone
does not link `mk9.exe` again, and the GPU plugin is copied beside it only when it does: after such a
build copy `external\rexglue-sdk\out\win-amd64\rexgpu-xenos.dll` into `mk9\out\build\win-amd64-release`.

## Source map (`mk9/src/mk9`)

| file | what |
|---|---|
| `frame.cpp` | the frame boundary: FEngineLoop::Tick (`sub_823621F8`), top/end hooks, frames run again (with the rest of GuardedMain's loop), the 60 frames/s cap |
| `crash.cpp` | crash reports: `crash.txt` and `crash.dmp` in the session folder when the game crashes (see "Crash reports") |
| `fx.cpp`, `guards.cpp` | the game's own crashes: its pool of effects making room when full, and checks put into its code where it wrote through a null pointer (see "Crashes in the game's own code") |
| `names.cpp` | the players' names over an online fight's health bars (see "Names over the fight") |
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
| `0x833CED48` | the fight's HUD (an object of 2168 bytes, 0 outside fights): +4 its top bar's offset down the frame, a float, -146 out of sight, 0 in place |

The C runtime's `rand()` is used only by FMOD (sound variations) and is not part of the simulation.

## Frame rate

At most 60 frames a second start (`frame.cpp`, `MK9_FRAME_CAP=<n>` for another cap, 0 for none), as on the Xbox 360.
The frames the game draws already wait for the GPU's vertical blank (the SDK's 60 Hz timer); the cap holds back
those that do not (loading screens and some menus ran at up to 69 a second) and never waits on one that did (it runs
a few microseconds faster than the blank, and makes up at most 2 ms of a late frame, so frames netplay holds back on
purpose stay held back). The game asks Windows for 1 ms timers: with the default 15.6 ms the SDK's blank thread,
which sleeps 1 ms at a time, delivered the blanks in uneven bursts. Measured on the boot to the title and the attract
fight: 62.7-70.4 frames/s and about 75 frames a minute off by more than 2 ms before, 59.3-60.1 and none after, with
the attract fight starting at the same second. The frame rate line every 600 frames counts frames under 15 ms and
over 20 ms after the one before, and the longest gap, and since 0.5.1 the window's presents a second
(`rex::ui::Presenter::PresentCount`, an SDK addition): what a frame counter overlay hooked into Present shows, and
how long a finished frame waited for its present (`TakePresentWaits`). Players' counters read 224 with the game at
60, and this PC counted about 300 from the first boot screens on, in every release so far: the SDK opens its
achievement toast (an ImGui dialog) when the game launches and keeps it for the whole run, and while any dialog was
open Xenia's ImGui drawer asked for the next paint at the end of each one, so the presenter painted as fast as it
could. The toast and the names overlay now ask for no continuous repaint (`ImGuiDialog::RepaintsContinuously`, an
SDK addition; the guest's frames repaint the UI): 60.0 presents a second through the menus, the select screen and a
fight with names, with the game at 60.0.

The SDK draws on Windows' first graphics adapter, and its Optimus/PowerXpress exports sit in `rexgpu-xenos.dll`,
where the drivers do not look: on a laptop with two the game ran on the slow one in the processor. `mk9.exe` exports
them itself, and sets `d3d12_adapter` to the one Windows counts as high-performance (unless it is set already).

## Tearing, window and resolution

The SDK's presenter (`ui/d3d12/d3d12_presenter.cpp`) hands every frame to the swap chain at once (sync interval 0):
the game keeps its own 60 frames a second and never waits for the display. With
`d3d12_allow_variable_refresh_rate_and_tearing` (its default) the swap chain also allows tearing, so that a G-SYNC or
FreeSync monitor can follow the frames; any other monitor shows a frame torn where it arrived mid-refresh. The
launcher's V-sync (on by default) passes `--no-d3d12_allow_variable_refresh_rate_and_tearing`: a flip model swap
chain that cannot tear, each refresh showing the newest whole frame. Nothing in the SDK logs it; the `--no-` form was
checked on `async_shader_compilation`, which then left out every "Resolve ... not copied" line (and stalled on its
shaders). A window's size is `window_width` / `window_height`, in logical pixels: the SDK multiplies them by the
display's scaling (1920x1080 came out 1920x1080 at 100 %).

`resolution_scale` 2 and 3 draw at 2560x1440 and 3840x2160, correctly (blood and damage too, the resolve fix above
working the same), at 60 frames a second once a fight runs, but they stall where 1x does not, the first times
something is drawn: on fresh saves, longest frames of 91 ms and 73 ms at 2x around a fight's start and its first hits,
212 ms (the fight loading), 96 ms and 82 ms at 3x; 1x stays under 18 ms throughout. Two games at different
resolutions stay in step: a lagged rollback match (50 ms, 30 of jitter, both attacking) with one at 2x and one at 1x
had every check equal, and in the fight the same script processes frame by frame (`scriptcmp.py`: 0 of 228 frames
with events differ); the 2x game's stalls (114 ms, 79 ms) showed in the other's frames too (longest 35 ms), as a
player who draws at more than 1x makes the opponent wait.

## Blood and damage textures

MK9 keeps the fight's blood and damage in textures it draws on the GPU: each fighter has several (128 to 512 pixels
square), whose colour the fighter's shader lays over its own by their alpha (pixel shader `74D59E4C54FD8912`, texture
fetch 5), and the floor uses a 2048x256 one (its blood, by all appearances). They are drawn from themselves: frame
after frame one of them is drawn back into EDRAM (at base 720, where the frame's own image is drawn) by a rectangle
that samples it, and resolved over itself; when a fighter is hit all of theirs are, with the fighter's mesh painting
the new damage over the old in texture space before the resolve.

The SDK compiles shaders and creates pipelines in the background (`async_shader_compilation`, on by default, as in
Xenia Canary) and drops a draw whose pipeline is not ready yet: on the screen that is an object missing for a frame or
two. At a fight's first hit the damage passes needed pipelines nothing had used before, so all of a pass's draws were
dropped, and its resolve copied what EDRAM held instead: the last frame's finished image (the arena, the HUD's green
health bars and their lettering), at the texture's pitch, in bands. Each later redraw drew that back into the
texture, so the fighter wore it for the rest of the fight: striped, see-through, in the arena's colours (a trace
caught the floor's texture copied the same way a moment later). It hits whichever pass meets a pipeline still
being created: Sub-Zero's at the scripted fight's first hit, Scorpion's in a run on the ROV render path.

Since 2026-10-29 (SDK patch, `D3D12CommandProcessor::IssueCopy`) a resolve copies nothing into guest memory when
every draw since the resolve before it was dropped, or when a dropped one sampled the memory the copy would write
(clears still happen). The texture keeps its last contents, without the damage of the hit whose pass was dropped,
and the log says `Resolve to 0x... not copied: n of the m draws before it were dropped while their pipelines were
being created`. Draws are still dropped while pipelines are created, so nothing stalls; what they left out no longer
reaches memory the game keeps. `--async_shader_compilation=false` also avoids it, with a stall instead each time a
pipeline is new (until the pipeline cache in the saves has them all).

Tested 2026-10-29 with the scripted fight (`tools\synctest.ps1` with `-Depth 0`, a fresh save each run): every run
before the fix showed the stripes; with it, three runs in three arenas showed none at frames 6200, 6400 and 7000,
and neither did a run on the ROV render path (`--render_target_path_d3d12=rov`).
Each run held back seven or eight copies (two at boot, two or three as the fight loads, three at the first hit) and
none later. The fight's frame rate lines stayed as before: 60.0 frames/s and none over 20 ms from frame 5400 to 7800.
The two-game rollback test with lag (`-Mode rollback -Lag 50 -Jitter 30 -AttackB -TraceScript`) agreed on every
check, and its fight's scripts were identical frame for frame.

Found with a GPU trace of the scripted fight (every draw's render targets and textures, every resolve, and which
resolve wrote each texture); the tracer is not part of the patch. The warnings about texture fetch constants of an
"invalid" type that fights log (two a frame, 1024x1024 DXT1 textures) start with the fight's loading, well before
the first hit, and are not part of this.

## Crash reports

`crash.cpp` hands an unhandled exception (any thread), a stack overflow (caught first, before the SDK's handler, with
64 KB of each guest thread's 16 MB stack kept back for it), `abort()`, `std::terminate`, a pure virtual call or an
invalid parameter to a thread made for it at start, which writes `crash.txt` (the exception and where: module and
offset, and `sub_82xxxxxx+off` when the address is in a recompiled function; host registers and call stack; the
crashed guest thread's registers and guest call stack; frames run, the launcher link's status and match, netplay's
state, the game's phase, screen and round; the modules) and `crash.dmp` (a minidump, about 1 MB, more after a stack
overflow) into `MK9_SESSION_DIR` (else `crashes\` beside the exe). The process then ends with the crash's code; the
launcher builds the player's report from these (LAUNCHER.md, "Crash reports"). `MK9_CRASH_TEST=<frame>[:abort|
:overflow|:throw]` crashes on purpose; all four kinds were tested.

## Crashes in the game's own code

Three of the first five player reports (0.4.0, 2026-10-28) were one crash: `sub_82A30C20`, the script command that
spawns an effect, writing to `[r3+260]` of a spawn that returned nothing. The game's effects (sparks, blood, fire, an
arena's ambience) come from a pool of 64 (the effect manager, vtable `0x82081C40`: in use at +640, free at +664, both
UE3 arrays); `sub_82606510` takes one off the free list and returns 0 once it is empty, and the game's code then went
on through the 0. Players saw it in scenes full of effects (Cyrax's air grab, Noob Saibot's teleport slam, the Church
arena were named). A scripted fight leaves 47-48 of the 64 free and every released effect goes back (no leak under
rollback), so the pool runs out only where the game itself spawns that much.

- `fx.cpp`: when the pool is empty, the oldest effect in use is released through the game's own release
  (`sub_82606650`) and the spawn tries again, on the main thread only (the job workers that also spawn run beside each
  other). The 10-second status line counts the effects taken, the takes that found the pool full and the effects that
  made room; the first time (and every hundredth) a line says so, which a crash report quotes.
- `guards.cpp` (`[[midasm_hook]]` in `mk9_codegen.toml`): where the game's code was seen writing through a nothing it
  takes the function's own way out instead, and logs it: the script command (`return -1`), `sub_828E4720` reading the
  new effect's id, and the rendering command `sub_82A569E0` setting values on an entry that was not there yet (a Test
  Your Luck fight's first seconds).

`MK9_FX_TRACE=1` logs how many effects are free as the game takes them; `MK9_FX_TEST_RECLAIM=<n>` makes room as if
the pool were full whenever fewer than n are free (tests: the scripted fight to frame 9000 with it at 62, plain and
under the sync test rolling back 4 frames every 10, ran with no crash and the sync test's notes as before).

Of the other two reports one was the rendering command above; the other, a read of `0x4` in the memory manager
(`sub_82B0C988`) during a Test Your Luck fight with rollbacks, is not explained yet.

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
draws differently). The fight's values count only in a fight and at its end (phases 8 and 11): until
the next fight sets them they keep the last one's, which a game booted since never saw (a lobby match
on 0.5.0 desynced at frame 0 between a fresh game and one that had played the title's demo fight).
Not while a fight loads (the VS screen, phase 1): each PC finishes loading when
it does and the fight's scripts start then, a frame or two apart from one PC to the next, drawing
and setting the round timer, until the fight's barrier puts them in step.

- **Lockstep** (default): a frame runs once every player's input for it is there; input delay 2.
- **Rollback** (`netcode=rollback`): frames of a fight run ahead on guessed inputs (up to 8) and a
  wrong guess puts the game back to the top of that frame and runs the frames since again with
  the real inputs (see "Rollback" below). Menus, loading and the match's end run in lockstep.

Spectators: a player's game writes its barriers, inputs and checks to the launcher's local feed
(TCP, `feed=127.0.0.1:port`); a spectator's game (`watch=1 feed=...`) reads every player from it
and plays the match in lockstep without a player of its own. The launcher relays feeds through
the server (FeedLink).

A game hears only its match: a UDP packet counts when it comes from the address set up for the
slot it names (the other game, the server's relay or the launcher's tunnel), others are dropped
(a game that played this PC before on the same port, or anyone who learned it, could otherwise
put inputs into the match or end it; the first and every thousandth are logged, and the crash
report counts them). Inputs and checks for frames more than 600 ahead of this game's are not
kept (a spectator's feed: an hour ahead).

## The launcher link

With `MK9_LINK=1` the launcher owns the game; see the header of `link.cpp` for the exact
`cmd.txt` / `status.txt` lines. The game boots to the hold point (main menu, fresh save) and says
`ready`; `start` meets the other games there, slot 0's autopilot steers both through the menus to
the mode's select screen (ONE VS ONE, TWO VS TWO, TEST YOUR LUCK), where each player picks with
their own controller; after the match it reports `result winner=N p1_rounds=.. p2_rounds=..`.
At the CONTINUE? menu (steered by the loser's controller, as on the console) `rematch` plays the
same fighters again, `rematch select=1` picks again, `park` quits to the main menu.

The hold point does not hold by itself: 60 s after the last press MK9 leaves its main menu for the
title, and about 30 s later plays its attract mode's demo fight. Until 0.5.0 the autopilot stood by and
the link went on saying `ready` (players' logs, 2026-10-29: the main menu at frame 1815, the title at
5326, the demo loading at 7281 and fighting at 7547); a lobby match started then began from the
demo's fight (DESYNC at frame 0) or from the title (the menus steered from another screen than the
other game's, which desynced at frame 510), and every later match of that lobby failed. Now the
autopilot taps the left trigger for 6 frames every 20 s at the hold point (the main menu does nothing
with it; the right stick, the left stick and the right trigger held it there as well, none of them
moving the cursor), a game that leaves the main menu all the same goes back to it (`Boot`, whose START
out of a demo fight now reaches the game: it never did) and says `booting` meanwhile, and a `start`
that comes while the game is not at its hold point waits for it instead of failing.

## Names over the fight

In an online match, and one watched, each player's account name stands just above their health
bar (`names.cpp`, drawn with the SDK's ImGui overlay in Segoe UI Black Italic, white with a dark
outline): P1's from where the left bar's lettering starts, P2's to where the right bar's ends, a
long name drawn smaller rather than run into the timer. The start line's `p1_name=` and `p2_name=`
carry them (offline matches and practice have none). They show only while the fight's top bar is
in place (the fight's HUD, `0x833CED48`, +4: the bar slides in from above as ROUND 1 is called,
and the names fade in once it has stopped), follow it if it slides away and go at once when the
fight is over (the phase leaves 8: the finisher). The frame's place in the window is the
presenter's, 16:9 as large as fits. `MK9_TEST_NAMES=Kitana|Sindel` shows names in any fight, for
screenshots (the game's own snapshots leave the overlay out; `tools\shot.ps1` takes the window), and
`Start-LinkLocal -Names "Kitana|Sindel"` puts them on the start lines as the launcher does; the log says
`mk9 names: ... over the fight` at the start, `mk9 names: up over the fight` once they show and
`mk9 names: drawn over the fight` once the overlay drew them.

The overlay is open only while names are up: the main thread opens and closes it on the UI thread
(`CallInUIThreadDeferred`) as the names come and go, and it asks for no continuous repaint
(`ImGuiDialog::RepaintsContinuously`, an SDK addition: every guest frame repaints the UI while a drawer is
attached). 0.5.0 kept it open from the start, beside the SDK's own achievement toast (see "Frame rate": the
two kept the presenter painting as fast as it could); with the overlay closed outside fights the game took 1.76
of the processor's cores at the main menu instead of 2.24.

Tested 2026-10-29 (0.5.0): a 4K window, the names after the bars' slide-in and not during it, and an
online rollback match driven through the link (both games showed them in round 1, `none` again after
the park).

## Testing on one PC

```
. tools\netplay-local.ps1; Start-NetplayLocal -Out <dir>        # two games, scripted menus and fight
. tools\link-local.ps1;    Start-LinkLocal -Out <dir> -Spectator -Attack -Rematch -Park
. tools\synctest.ps1;      Start-Synctest -Out <dir> -Depth 4 -From 5400   # rollback restore test
. tools\netplay-local.ps1; Start-NetplayLocal -Out <dir> -Mode rollback -Delay 1 -Lag 50 -Jitter 30 -AttackB -Sites
python -I tools\netcmp.py a\net.log b\net.log                  # frame logs of two games side by side
python -I tools\randcmp.py a\rand.txt b\rand.txt               # MK9_TRACE_RAND random draw traces
python -I tools\scriptcmp.py a\script.txt b\script.txt         # -TraceScript: what scripts did, frame by frame
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
5. **Whole frames.** GuardedMain's loop runs more than `FEngineLoop::Tick` each time round: before
   the tick it counts the frame, reads the pads (`sub_829D5230`; the tick only reads what that put
   in the game's pad state) and the key presses (`sub_829D49F0`) and updates the game's systems
   (`sub_8239D550`), after it it measures the frame. A frame run again runs all of it
   (`frame.cpp`, `RestOfLoop`), and what the key presses had reported goes back with the frame
   (`input.cpp`). Until 2026-10-28 only the tick ran again: a rollback from frame n to r played
   frames r+1..n on the pads read before r, so inputs that came in for them took effect only after
   n, and the two games of a lagged fight drifted 1-3 frames apart.

The sync test (`MK9_SYNCTEST=4`) goes back 4 frames every 10 frames through a scripted fight: 420
rollbacks and 1,680 frames run again without a crash, worst rollback about 70 ms (frames run
again render too). `MK9_SYNCTEST_VERIFY=1` compares every tracked page after the first restore
with a copy taken at that frame's top (only foreign audio pages differ) and lists what a frame
run again wrote differently. Its own pass/fail ("frames differed") counts every byte, D3D fences
included, so it always reports differences; the two-game test is the real check: with 50 ms lag,
30 ms jitter and both players attacking, two games roll back 23-31 times each (up to 8 frames back)
over the first 80 seconds of the fight and their checks agree (162-172 checks a run), and so do
their scripts, frame for frame (`-TraceScript`, `scriptcmp.py`: every process made and every
number drawn on the same frame in both games).

Diagnostics: `MK9_RB_TRACE_FOREIGN=1` (who writes which pages first, stacks of foreign writers),
`MK9_GMALLOC_TRACE=1` (frees traced, double frees explained), `MK9_NET_SITES_LOG` (draws per
call site at every check; `-Sites` in `netplay-local.ps1`), `MK9_TRACE_SCRIPT` (every script
process made and every script random draw, with the frame and the rollbacks; `-TraceScript`, and
`tools\scriptcmp.py` compares two games' frame by frame as their rollbacks left them).

## Known gaps

- Rollback is tested on one PC over loopback with simulated lag (see "Rollback"), not yet over the
  internet. Frames run again also render; a long rollback costs about 17 ms a frame (16.5 ms measured by
  the sync test, which now logs it; 13 ms with the SDK's vsync off while they run, not kept: unproven).
- **Rollback went out of step under lag** in 0.3.x builds and the first 0.4.0 commit (found and fixed
  2026-10-28): the two-game test above with `-Lag 50 -Jitter 30 -AttackB` stopped with DESYNC in 9 of 11
  runs, most at fight frame 1590. Frames run again ran only the tick, not the rest of GuardedMain's loop,
  which reads the pads ("Rollback", 5), so inputs inside a rollback's frames took effect only after them
  and the two games' fights drifted 1-3 frames apart from the first rollback on; a check caught it when a
  draw fell in between. At 1590 that was a fighter's own script (process `1000096C`; draw site
  `82A23504/824790DC` is the script random native `sub_824705F8` keyed by it, not `sub_824790A8`) drawing
  `random(100)` twice a frame for 10 frames, from a frame or two later in one game. Not FMOD: once frames
  run again run the whole loop, the scripts of both games do the same on the same frames (`scriptcmp.py`).
  The stop in lockstep at the end of loading (3 runs of 15) was the checks comparing the fight's
  first script draws and round timer, which each PC makes when its loading ends: no checks while a fight
  loads now. Since: 4 lagged runs in a row with no DESYNC and 0 check differences, lockstep agreeing.
- Sounds started in frames a rollback takes back keep playing (FMOD is not rolled back).
- Seen once in about twenty test boots (2026-10-28, three games starting at the same moment): a crash in
  `main()` before the first frame (`sub_823669D8`, a write through a null pointer, the job workers then
  reading through one too). Not looked into yet.
- Four-player TWO VS TWO (slots 2-3) is in the netplay core but untested.
- The autopilot steers menus by fixed steps from a fresh save's cursor; the roster grid is not
  mapped yet, so the launcher cannot pick a named fighter (players pick in the game).
- **A fighter striped and see-through from a fight's first hits** in every build before 2026-10-29, and in Xenia
  Canary (found and fixed 2026-10-29, see "Blood and damage textures"): draws dropped while their pipelines were
  being created let a resolve copy the last frame's image into a blood and damage texture the game draws from
  itself. None of the settings tried first changed it (`render_target_path_d3d12=rov`, `readback_resolve=full`,
  `native_2x_msaa=false`, `gpu_allow_invalid_fetch_constants=true`,
  `texture_cache_memory_limit_render_to_texture=256`, `MK9_SIMHEAP=0`): none of them stops draws being dropped.
