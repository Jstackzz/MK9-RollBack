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
ask for no continuous repaint: see "Names over the fight"; the frames a rollback runs again drawn only
into small targets and waiting for no vertical blank: see "What a rollback costs"). On a fresh
checkout of the SDK: `git -C external\rexglue-sdk apply ..\..\patches\rexglue-sdk-v0.10.0.patch`; the
patch is `git -C external\rexglue-sdk diff --ignore-submodules`. A change to the SDK's GPU code alone
does not link `mk9.exe` again, and the GPU plugin is copied beside it only when it does: after such a
build copy `external\rexglue-sdk\out\win-amd64\rexgpu-xenos.dll` into `mk9\out\build\win-amd64-release`.

## Source map (`mk9/src/mk9`)

| file | what |
|---|---|
| `frame.cpp` | the frame boundary: the top of GuardedMain's loop (its first call, `sub_82366BF8`), before it reads the pads; top/end hooks around FEngineLoop::Tick (`sub_823621F8`), frames run again (the whole loop), the 60 frames/s cap |
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
| `0x833C6C74` | the last round's winner, 1 or 2: set as a round ends, seconds before the round number goes up, and kept until the next round's end (into the next fight) |
| `0x833C7A38`, `0x833C7A3C` | rounds won by player 1 / 2 as the game counts them in a match played on one PC (final once CONTINUE? is up); 0 all through a lobby match |
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
:overflow|:throw]` crashes on purpose; all four kinds were tested. A debugger follows host pointers and finds none
into guest memory, so `crash.dmp` also takes the crashed guest thread's stack (from 1 KB below r1 up, 256 KB at most)
and 1 KB around every guest address in its registers; `crash.txt` says where guest memory sits (`guest memory at
host ...`: guest address a is host address that plus a).

A game that stands still says where too (since 0.5.5, as MK3D Online's game link watches its frames): a thread of
`crash.cpp`'s, started with the first frame, looks every 250 ms, and when no frame started for 10 s while netplay is in
sync and not waiting for the others' inputs (`net::Waiting`; a barrier is not in sync), it writes `hang.txt` beside
where `crash.txt` would go: where the main thread's host code is (its registers taken while it is suspended a moment,
its stack walked once it runs again) and its guest registers and call stack, the rendering thread's
(`engine::RenderContext`), the frames run and the game's and netplay's state; the log says `mk9 watch: no frame for
10 s at frame f: the game stands still (hang.txt), its main thread at ...`, once, until frames go on again (`mk9 watch:
frames again after n s`, and it becomes `hang-over.txt`). A look a second or more after the last one (a debugger, the
PC asleep) starts the count again. The launcher reports a game closed with `hang.txt` there. 0.5.3's three matches
whose game was closed minutes in (exit code 0) said nothing of why. `MK9_CRASH_TEST=<frame>:hang` stands still 15 s.

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
fighters' numbers alone). A process is told apart by its object id counted from the id the next
object took at the last barrier: two PCs' menus and loading do not make the same number of objects
before a fight (0.5.3 and 0.5.4: the same fighter script was process `10000BB7` in one game and
`10000BBA` in the other, `100008AB` and `100008A2` in another match whose next object ids were 9
apart), and the ones one PC made more came before the fight's scripts. At a fight's barrier every PC also takes
slot 0's frame counter of the game state (`0x833C6C54 + 112`, `game.cpp`), which counts from when the fight began
loading: bit 0 of it decides each frame whether P1's or P2's script processes run first. The other globals two PCs
come to a fight with apart (the game state's frame count from boot at `+108` and its copies, GCurrentTime at
`0x833DD518`, the words at `0x833AFA84` that look like a generator's state) steer nothing: with each moved in one game
of a test pair, a lockstep fight played to its end the same frame for frame. Each PC sends its own input with every
stick axis within 25 of 127 and every trigger within 25 of 255 read as untouched (`Quiet`, `netplay.cpp`): a stick that
drifts a few steps off centre changed the input every frame, and the other PC rolled back on each one (a player's
0.5.5 opponent: up to 8 frames at almost every frame; a test pair drifting by up to 7 rolled back on 64 % of its frames,
the same pair with the filter as often as without drift). Every 30 frames of a fight and
of its end (phases 8 and 11) both sides compare a state hash: the round timer, the round, the rounds
won and the draws made at every call site so far (a fight that goes differently draws differently).
Not outside them: until the next fight sets them the fight's values keep the last one's, which a game
booted since never saw (a lobby match on 0.5.0 desynced at frame 0 between a fresh game and one that
had played the title's demo fight); the menus' and select screen's draws go with what each PC has
loaded by then (0.5.3: two matches stopped before their fight, one PC's select screen with 26 draws
more at `82383B38`), and what the players pick there is in their inputs; and while a fight loads (the
VS screen, phase 1) each PC finishes loading when it does and the fight's scripts start then, a frame
or two apart from one PC to the next, drawing and setting the round timer, until the fight's barrier
puts them in step and starts every draw anew.

What a game sees must not depend on its PC. The GPU's occlusion queries (how much of something got
drawn) decide what UE3 counts as seen, and some of the game's effects draw from their streams only
while seen; a graphics card answers them by its own rasterizing, at the player's resolution and on
its own time. Until 0.5.2 the SDK answered them from the host GPU (`occlusion_query_enable`, on by
default), and two players' games went out of step in nearly every online match (10-28: 29 desyncs
against 7 matches played out; 0.5.1: all 12 matches of its first evening lost), while two games on
one PC, one graphics card, never did. One game answered with Xenia's fixed count and the other by
the GPU: out of step at frame 1740 of the lagged two-game test, every script process the same frame
for frame (the effects' draws differed, not the fighters'). The game now sets the fixed count
(`query_occlusion_fake_sample_count`, 1000) for every query, unless the command line says otherwise.
A desync's log (and so its report) lists the failed check's draws by call site, grouped by return address
(`mk9 net: draws at frame f by site: 82A23504: 824790DC=40 8247063C=8 ...`, 4 lines at most; a script's
draws are at `8247061C ^ ((process id - next object id) << 4)`, the next object id at the fight's barrier, on its
`match N in sync` line since 0.5.4; until 0.5.5 `8247061C ^ (process id << 4)`): the two players' reports side by
side show what drew differently. Since 0.5.4 each check's round and rounds won are beside its timer in the net log
(`check <hash> timer 90 round 1 won 0-0 rand <state>`): what of a check differed when the draws did not.

- **Lockstep** (default): a frame runs once every player's input for it is there; input delay 2.
- **Rollback** (`netcode=rollback`): frames of a fight run ahead on guessed inputs (up to 8) and a
  wrong guess puts the game back to the top of that frame and runs the frames since again with
  the real inputs (see "Rollback" below). Menus, loading and the match's end run in lockstep, and
  a fight's first 30 frames take no input on any PC, so no rollback goes back into them ("Known gaps").

With input delay d, what a player's pad holds at the top of frame f is their input for frame f+d on
every PC, and frame f+d plays on it. A frame's top is the top of GuardedMain's loop (`frame.cpp`):
netplay settles the frame's inputs there, and the game reads its pads after that, before the frame's
tick. Until 2026-10-29 a frame's top was the tick's call, which GuardedMain makes after reading the
pads, so every frame played on the inputs of the frame before: every match, lockstep and rollback,
played a frame behind its delay (delay 2 played as 3). With `-TraceScript` each game's trace says
what the game read before each tick (`pads` lines), and `scriptcmp.py` holds that against the game's
`net.log`: on every frame where an input changed, the new one, that frame.

Spectators: a player's game writes its barriers, inputs and checks to the launcher's local feed
(TCP, `feed=127.0.0.1:port`); a spectator's game (`watch=1 feed=...`) reads every player from it
and plays the match in lockstep without a player of its own. The launcher relays feeds through
the server (FeedLink). One that watches a match under way gets its feed from the start, all at
once: the next match's barrier comes with this one's, and its READY is held until this barrier is
over, and inputs that came during a barrier count. Until 0.5.3 neither: the next match's READY took
the place of this one's (the spectator waited at the first barrier until it gave up after 60 s,
"Known gaps"), and after a barrier it waited for inputs it already had.

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
. tools\link-local.ps1;    Start-LinkLocal -Out <dir> -Netcode rollback -Delay 1 -Attack -Rematch -TraceScript -Extra "MK9_NET_LAG=50;MK9_NET_JITTER=30"
. tools\synctest.ps1;      Start-Synctest -Out <dir> -Depth 4 -From 5400   # rollback restore test
. tools\netplay-local.ps1; Start-NetplayLocal -Out <dir> -Mode rollback -Delay 1 -Lag 50 -Jitter 30 -AttackB -Sites
python -I tools\netcmp.py a\net.log b\net.log                  # frame logs of two games side by side
python -I tools\randcmp.py a\rand.txt b\rand.txt               # MK9_TRACE_RAND random draw traces
python -I tools\scriptcmp.py a\script.txt b\script.txt         # -TraceScript: scripts and pads read, frame by frame
python -I tools\feedreplay.py --seed <start line's seed> <session>\net-<match>.log   # a real match played again:
                                     # a linked game given the spectator's start line with feed=127.0.0.1:7302
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
   between tasks; the pool's threads in their DoWorkEvent wait with no work given (work given once
   the hold is complete waits for its end, the next frame's). PhysX's events (start, done) are
   kept with every frame and set back with it (`SaveEvents`), so a step that was running across a
   frame's top runs again; so are the pool threads that had work given at a frame's top
   (`SavePool`). Until 0.5.1 a match's rematch kept none: each match
   numbers its frames from 0, the kept states were trimmed from the lowest frame, so the last
   match's stayed and every new one went at once, and rollbacks in a rematch's fight left PhysX's
   events as they stood (the key presses had the same fault, "Known gaps").
2. **Whose memory.** `GMalloc` (the MemoryManager at `0x83423BA8`) is routed from boot on
   (`gmalloc.cpp`): the simulation's threads allocate from the simulation heap (a TLSF heap in a
   region of its own, its bookkeeping inside it), FMOD (from its code or UE3's file callbacks for
   it) from the audio heap, which rollback never tracks, and every other thread from the engine
   heap. While armed the simulation never writes the engine heap's bookkeeping: its frees of
   engine blocks wait for disarm, unless a rollback took the frame back. A simulation block
   another thread writes (FMOD's file handles, a loader's buffer) is that thread's from then on.
3. **What a restore leaves alone** (volatile bytes): the rendering command ring and its control
   block, the D3D device, every critical section outside the simulation heap (a lock's state never
   goes back), the GPU callback's frame-time globals, the engine heap's own state, FMOD's table
   of the memory it uses (`0x834B7F78`, below) and XAudio2's globals (`0x833C3AC0`, `0x834C19A4`).
   A restore writes none of them: it copies only a page's other bytes (until 2026-11-03 it copied the
   page whole and then those bytes from a copy taken just before, which undid what another thread
   wrote to them in between: "Known gaps").
   Pages other threads write first are "foreign" (never put back), but a page holding volatile
   bytes is logged whoever writes it, and so is a page of the executable's data with a lock on it
   (it holds the simulation's globals too). A page elsewhere with only locks on it is its writer's
   like any other: logged for its locks, XAudio2's engine object went back under its mixing thread
   ("Known gaps").
4. **The host.** `ReadFile` into a write-protected guest page fails in the kernel instead of
   faulting: the SDK's file read takes such pages first (`patches/`).
5. **Whole frames.** GuardedMain's loop runs more than `FEngineLoop::Tick` each time round: before
   the tick it counts the frame, reads the pads (`sub_829D5230`; the tick only reads what that put
   in the game's pad state) and the key presses (`sub_829D49F0`) and updates the game's systems
   (`sub_8239D550`), after it it measures the frame. A frame's top is the loop's top (its first
   call, the time base read `sub_82366BF8` from `0x82366864`), so what is kept of a frame is from
   before its pads were read, a frame run again runs all of the loop (`frame.cpp`, `BeforeTick`,
   the tick, `AfterTick`), and what the key presses had reported goes back with the frame
   (`input.cpp`). Until 2026-10-28 only the tick ran again: a rollback from frame n to r played
   frames r+1..n on the pads read before r, so inputs that came in for them took effect only after
   n, and the two games of a lagged fight drifted 1-3 frames apart. Until 2026-10-29 a frame's top
   was the tick's call, after the pads were read (see "Netplay": a frame behind the input delay).

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

### What a rollback costs

A rollback runs its frames again and then the frame due, and all of it before the next vertical
blank, or the game misses it and the fight slows down: what players of a lagged match feel as slow
and muddy. Every 10 s of a fight that rolls back the log has the rollbacks (`mk9 net: n rollbacks
in 10 s, m frames run again (x ms a frame)`, split into putting memory back, the game's own frames,
waiting for the rendering thread and the page logs), what keeping frames costs every frame (`frames
kept for rollback`), and from the GPU what the frames run again skipped (`Frames nobody sees`).
Since 0.5.1 (numbers from the two-game test: 50 ms lag, 30 of jitter, both attacking, 30-40
rollbacks every 10 s of the fight's busiest stretch):

- **Keeping frames.** A fault in a 64 KB block of the simulation heap that frames write densely (4
  pages or more, measured every 16th frame) takes every clean page of the block at once and lifts
  the protection of runs of pages in one call (`MK9_RB_NO_AHEAD=1`: page by page), undo logs keep
  their buffers from one frame to the next, and a restore puts each page back once, from the oldest
  log, protecting in runs: the fault handler went from 11 ms a frame to about 2, a restore from
  9.5 ms to 3.
- **Frames nobody sees.** Only the frame due is shown, yet the GPU drew every frame run again in
  full, and each waited for a vertical blank: MK9's swap ends in a `WAIT_REG_MEM` on a word of the
  D3D device that the guest's vertical blank interrupt clears when it flips the image
  (`sub_82BF65B0`, from the queue `sub_82BF66D0` fills, one flip a blank), so the GPU ran one frame
  a 60 Hz blank and the rendering thread waited behind it for room in the command ring: a frame
  run again took 14.1 ms, 13 of them that wait, rollbacks took up to 108-114 ms, and that stretch
  of the fight ran at 53 frames a second (its longest frame 115-121 ms). A rollback now marks
  where its frames start and end in the GPU's command stream
  (`IGraphicsSystem::SkipDrawsFromNow`, an SDK addition; the rendering thread is held at both
  marks; `MK9_RB_DRAW_ALL=1` draws them all), and between the marks the GPU skips the swaps and
  every draw to a target of at least 640x320 (the frame's image, its half-size copies, the
  1014x1014 shadow map), copies nothing in those passes' resolves (clears still happen), and when
  a wait is not met gives the guest its vertical blank at once (up to 3 a wait). The small targets
  are drawn as ever: the fighters' blood and damage textures (128 to 512 square), the floor's
  (2048x256), the quarter-size effects. A frame run again now takes 6.5 ms (3.9 the game's own,
  1.7 the rendering thread, 0.9 the page logs), the longest rollback 37-40 ms, and the same
  stretch ran at 58 frames a second (its longest frame 46-60 ms); the checks and the scripts of
  both games agreed as before. The two games' blood differs a little either way
  (snapshots of both at the same frames: a spot on a fighter's arm, the floor's splashes; 0.05-0.6 %
  of the screen with the skip, 1.1-1.5 % with every frame drawn): the GPU's textures do not go
  back, so the blood of a hit a rollback took back stays painted.
- **The frame's input first.** Until 0.5.5 a frame's own input went to the other players after the
  frame's rollback had run its frames again (25 ms or more for 3 frames back), and all that time
  they guessed it and ran ahead on the guess: a rollback on one PC put off the input that the other
  needed, and so brought on its rollbacks too. It now goes out at the frame's top, before anything
  runs again (`SendOwn`, `netplay.cpp`). The two-game test's busiest 10 s: 37 and 27 rollbacks, 95 and
  65 frames run again, against 40 and 32, 95 and 80 just before; the test's jitter (30 ms either way)
  makes most of its rollbacks, and a real match's rollbacks come more from the other PC's.
- **Keeping the sides even.** A PC that runs ahead of the other guesses the other's inputs further
  ahead, and each wrong guess is a rollback that long; the PC behind never guesses. A PC more than
  a frame and a quarter ahead holds a frame back, as often as every other frame (`Pace`,
  `netplay.cpp`): the next frame starts a whole frame later than the frame cap would start it
  (`frame::HoldNext`; the screen shows the frame before once more). How far ahead is averaged over 8
  frames, with what the frames it held back have not yet taken off the average counted as if they
  had (half of each shows the next frame, half a round trip later, once the other PC heard of it).
  Until 0.5.6 a PC held back one frame in 20 at most, by sleeping 16.7 ms in the frame, and the cap
  made most of that up: it starts a late frame's next one as if it had been on time, so a frame of 6
  ms of work lost 5 ms, not a frame (a test game holding back every other frame that way still ran 52
  frames a second against one at 50). Against a PC that ran fewer frames a second (a player's 0.5.5
  match: 49) the faster one ran ahead until it guessed 8 frames ahead, its rollbacks 4.5 frames long
  on average. The two-game test with one game held to 50 frames a second (26 ms each way, 15 of
  jitter, delay 2, both mashing; 0.5.5 against this): the faster game held back 7-8 frames a second
  and stayed 0.9 frames ahead; 1,408 frames run again in the fight instead of 2,092, rollbacks 2.5
  frames long on average instead of 3.7, none longer than 5 frames instead of 81 of 6-8. The log has
  it every 10 s (`frames ahead of the others on average`, the frames held back and the frames that
  waited at the 8-frame limit); `MK9_TRACE_PACE=<file>` writes every frame's numbers.

Diagnostics: `MK9_RB_TRACE_FOREIGN=1` (who writes which pages first, stacks of foreign writers,
each page a simulation thread writes after another thread wrote it first), `MK9_RB_TRACE_SHARED=1`
(each page another thread writes first that is logged anyway for the volatile bytes on it, with that
thread's stack: what a restore puts back under it), `MK9_RB_TRACE_FMOD=1` (pages FMOD's code writes
on a simulation thread),
`MK9_GMALLOC_TRACE=1` (frees traced, double frees explained), `MK9_NET_SITES_LOG` (draws per
call site at every check; `-Sites` in `netplay-local.ps1`), `MK9_TRACE_SCRIPT` (every script
process made, every script random draw and the pads read before each tick, with the frame and the
rollbacks; `-TraceScript`, and `tools\scriptcmp.py` compares two games' frame by frame as their
rollbacks left them).

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
- **Every netplay match played a frame behind its input delay** until 2026-10-29 (found and fixed then):
  GuardedMain reads the pads before it calls the tick, and the tick's call was a frame's top, where netplay
  sets the frame's inputs, so the tick of frame f played on the inputs of f-1, in lockstep and rollback alike
  (a match at delay 2 played as 3). Each frame's inputs were sent, awaited and guessed for that frame all
  along, so nothing waits or rolls back more without the extra frame: the lagged test's menus run at the same
  40 frames/s as before, and its fights roll back as often (21-32 times a game, up to 8 frames back). A
  frame's top is now the top of GuardedMain's loop ("Rollback", 5). The lagged rollback test: 3 runs in a row
  with no DESYNC, 0 of 171 checks different in each fight, the fights' traces (scripts and pads) the same frame
  for frame in both games, and on every frame of a fight where an input changed (113-115, a quarter of them run
  again by a rollback) both games read the new input on that frame; the same test on the old frame top read
  every one a frame late. Lockstep agrees (166 checks, the same traces), and so does a launcher-link match
  and its rematch with the autopilot steering the menus (the same results). Found on the way: the key presses
  rollback keeps (`input.cpp`) were all thrown away from a match's rematch on (each match numbers its frames
  from 0, the last match's kept frames stayed, and every new one was trimmed at once), and the first frame a
  fight keeps had none; both fixed.
- Sounds started in frames a rollback takes back keep playing (FMOD is not rolled back).
- **A rollback into a fight's first frames crashed the game** in every release with rollback until 0.5.1 (found
  and fixed 2026-10-29): the other player saw it stop answering, and the launcher was left at CONTINUE?.
  Lagged launcher-link tests (`Start-LinkLocal -Netcode rollback ... -Extra "MK9_NET_LAG=50;MK9_NET_JITTER=30"`,
  possible since then) roll back from the fight's first frames, as players pressing through a fight's start do;
  the two-game test's fights only rolled back once the scripted attacks began, a second or more in. Every such
  run crashed the game that rolled back, in the frame it ran again first (frame 1 of the fight, back from 3 to
  9), in FMOD (`sub_82E85748`, from the script's sound native `sub_82A38050` through `sub_82E7D078`), reading
  through freed memory; the same with the GPU skip and the block copies off, and the same in a build of
  f6a4cd2. In the fight's first frame the game sets the fight's sounds up through FMOD on the main thread, and
  FMOD's heap is not rolled back, but part of its state is in tracked memory (`MK9_RB_TRACE_FMOD=1` lists the
  pages FMOD's code writes on a simulation thread): a table of the memory it uses, an entry a megabyte at
  `0x834B7F78` (built by `sub_82EF4E78`, written only by FMOD's allocations and frees), three pages of the
  executable's data more, and simulation heap pages written by the game's code FMOD calls back. A restore of
  that frame put that part back and left the rest. Every crash went back to frame 1; rollbacks to frame 5 and
  later never crashed (60 s of a fight rolling back from frame 5 on, 36-62 times every 10 s, with FMOD's table
  left alone), and leaving the table alone did not save frame 1. Since 0.5.1 the first 30 frames of a rollback
  fight take no input on any PC (`kQuietFrames`, half a second inside the fight's intro, `netplay.cpp`), so no
  rollback goes back into them, and FMOD's table is left alone with the engine heap's state (`engine.cpp`). The
  lagged launcher-link test then played a match and its rematch to their KOs (445 and 431 rollbacks, up to 7
  frames back) with the same results on both PCs, every check equal and both games' scripts the same frame for
  frame (0 of 6,283 and 0 of 5,427 frames differ), the rematch's PhysX events kept and put back too (`SaveEvents`,
  "Rollback" 1). On the way: a frame run again called the tick with the return address of the call before it
  (`0x823668D0`, not GuardedMain's `0x823668D8`; `frame.cpp`), which only the scripts' traces saw.
- **Matches stopped as out of step when they were not** until 0.5.3 (found in the server's desync reports and
  fixed 2026-10-29): 14 lobby matches were lost to desyncs on 0.5.2's first evening, 13 of them between the same
  two players. Four of the eight reports that listed their draws had the same draws on both sides, and in one
  (`c5e6fb59f4df`) the game's first run of frame 1920 had sent the other player's very check: a rollback to 1920
  after that check had gone out ran the frame again and sent a second one, with the same draws, round timer and
  generator. A rollback takes back the draws of the frames it puts back (`game::RolledBack`), and a call site whose
  draws were all taken back stayed in the counts at 0; the check hashes every site with its count, so that game
  had one site more than a game that never drew there. Since 0.5.3 such a site goes. Scripted tests never showed
  it: their attacks come again a frame later and draw at the same sites before the next check. A desync's report
  now lists the draws by call site ("Netplay"); until then by the 64 KB of code, and for checks below about frame
  500 not at all (each match numbers its frames from 0, and the 16 checks kept for it were the menus' later ones).
  `a5e3862d9529` went out of step at the first check, frame 30, before either game had rolled back and with no
  input from either player (explained in 0.5.3's reports, the next entry). The lagged two-game test stays in step
  with one game's audio queued 64 frames ahead (`audio_maxqframes`), with its memexport readback off, or drawing at
  3x3 resolution.
- **Fights stopped as out of step when their scripts drew alike** until 0.5.5 (found in the server's desync reports
  of 0.5.3 and 0.5.4, fixed 2026-11-03): a script process's draws were keyed by its object id, and two PCs' menus
  and loading do not make the same number of objects. In 0.5.3 two fights stopped at their first check, frame 30,
  the same fighter script drawing as process `10000BB7` in one game and `10000BBA` in the other; in 0.5.4's first
  lobby match the fight stopped at frame 1020 as round 1 began, its draws alike but at processes `100008AB` and
  `100008A2`, the next object ids at the fight's barrier `100008C7` and `100008BE`, 9 apart as well. Draws are now
  keyed by the id counted from that barrier's (`g_id_base`, `game.cpp`). Two more of 0.5.3's matches stopped at
  the select screen, one PC with 26 draws more at `82383B38` than the other: the menus' draws go with what each
  PC has loaded, and only fights are checked now ("Netplay"). The lagged two-game test then had no check in the
  menus and every one of the fight's equal, the fight's scripts the same frame for frame.
- **Fights went out of step when two PCs' loading took a different number of frames: the fighters' scripts ran in
  opposite orders, and one PC spawned a pair of effect objects a frame before the other** (found in the server's
  desync reports of 0.5.4 and reproduced in the two-game test, fixed 2026-11-03). 0.5.4's first evening:
  five of the six lobby matches two players started, 47 ms apart through the relay, went out of step (none
  finished; the sixth was the id offset above). With the same next object id at the fight's barrier, a script
  process (`100009E8`) drew once in one game and never in the other by frame 1080; one (`100007EE`) drew 6 times
  against 8 by frame 900; the site `82A25B7C/829FA8C0` drew 10 times against 11 by round 2; and a process made
  during round 2 had an id 4 higher in the game that had rolled back 79 times than in the one that had rolled back
  31 times. Twelve script processes and three natives drew in those fights; the two-game test's scripted fight
  (walking and jabbing) has one drawing, and never went out of step in any pairing or arena tried (Scorpion and
  Sub-Zero, Ermac and Freddy Krueger; The Living Forest, The Pit, its Bottom and Day, Rooftop at dawn and by day).
  With both players mashing (specials' motions, strings, throws, enhanced moves, X-rays; 40-84 rollbacks every 10 s;
  scratchpad `mash.py`) one seed drifted in 11 runs of 13, every check still equal: with ids and addresses left
  out the two fights' scripts did the same on every frame but five, where one game made a pair of objects
  (`sub_82AB2A64` through `sub_829EDC20`) a frame before the other, or the other not at all, and every later id was
  2 apart, so every later script process's draws went to other sites. The pair comes from the spawn list
  `sub_829F0BE8` goes through (fx.h): at the frame they parted both games had the same entries there with the same
  flags and values, in another order, and the order decides whether an entry is flagged again and spawns the next
  frame. It looked like a rollback's doing: the orders parted at the same moments of the fight from run to run (the
  hit at about frame 1302 that spawns five such pairs) in the game that had run frames again, the same fight in
  lockstep never parted, and the list's order changed in no call of the game's around it (a watch on
  `sub_829F44A0`, `sub_829EB328`, `sub_829ED498` and the spawns). Nothing a replay was suspected of changed it:
  frames run again drawing everything (`MK9_RB_DRAW_ALL=1`), the memexport readback off, XAudio2's mixing stopped
  for the whole fight, the job workers kept out (`sub_827783B0` answering nothing), the list gone through by its
  objects' ids. A compare of all tracked memory at a frame's top after a straight run and after the same frames
  run again (a wrong guess first, in the sync test) found GPU fence counters and the simulation heap's free lists
  different, nothing of the lists. Traces of every entry put on the lists showed it: a script command
  (`sub_829F0A48` / `sub_829F0B20`) takes an effect's entry out (`sub_829ED348`) and puts it at the list's end
  (`sub_829ED270`), and at the hit P1's and P2's main processes each do that to their effects, so the list ends in
  the order the processes ran. The script VM runs 21 lists of processes in order (`sub_82A164C8`, at `0x8342CDFC`),
  and every frame a script command (`sub_823BCC70`) puts P1's processes in list 12 plus bit 0 of the game state's
  frame counter (`0x833C6C54 + 112`) and P2's in the other. That counter counts from when the fight began loading,
  and the two games came to the fight's barrier with it one apart (531 and 532 in a run that parted, 534 and 533 in
  another; rollback puts it back like any other word): every frame of the fight they ran the fighters' scripts in
  opposite orders, which shows only where both queue effects on one list in one frame. A run in step had the lists
  alike on every frame. Since 0.5.5 the barrier's words carry the counter and every PC takes slot 0's
  (`game::Agree`): the mash seed that had gone out of step in 11 runs of 13 stayed in step in 4 runs of 4, its two
  games coming to the barrier 1 to 3 frames apart (213 and 210 in the first) and fighting from slot 0's count.
  The reports' other cases fit this too (a process that drew more in one game: the pairs shift every later id, and
  the fighters' main scripts, made by `sub_823B3EB8`, draw in bursts of 2 a frame, so the reports' differences of 2
  and 10 are one and five such frames). Still open from this hunt: a sync test that runs the frames it takes back
  with other inputs first, where a game-flow script (made by process `1000033F`) slept 28 ticks against the
  straight run's 1; FMOD's thread writing game memory through the game's file callbacks (`sub_82602D00` /
  `sub_82604168` into `sub_82728A38`) at moments of each PC's real time, which made bytes of the game state's page
  (`0x833C6000`) volatile for good. That sync test's crash, Reptile and Sub-Zero, is the next entry.
- **Rollbacks put back memory from frames they took back and undid other threads' writes: the sync test crashed in
  the rendering thread, the mash test froze** (found and fixed 2026-11-03): the sync test that runs the frames it
  takes back with an attack nobody pressed first, goes back again and then runs them with the real inputs (a "ghost
  pass") crashed in one run of three, Reptile and Sub-Zero at frame 7425: the rendering thread freed a simulation
  heap block that was free already and then called through a freed skinned mesh's dynamic data (`sub_829396A8`, the
  render command that swaps the data a mesh proxy holds at +236 and deletes the old, queued by `sub_82944228`; the
  old one's 16 bytes were by then another object's).
  Found with a diagnostic build that keeps every simulation heap allocation and free with its frame, thread and guest
  stack, checks the heap's blocks and free lists after every restore and every frame run again, lists every word
  still pointing at a block freed twice, and compares all tracked memory after the ghost pass's second restore with
  the first (they must be the same). Two faults at a frame's top, between closing the frame and going back:
  1. A page first written after a frame closed is kept in a pending log that the next frame opened takes as its
     own. When a restore came in between and put that page back, the kept copy was still the one from before the
     restore, and it went to the frame gone back to: a second restore to that frame wrote back memory of the frames
     the first one took back. Such writes come from threads that go on while the simulation is held: FMOD's stream
     thread writes UE3's file readers in the simulation heap through the read callback (`sub_82602D00` ->
     `sub_82604168`), XAudio2 and the GPU's interrupt thread their globals on logged pages, pool threads that ran on
     (2. below); and in the simulation heap one such write takes the whole 64 KB block it falls in. Caught in one
     run: the restore to frame 5994 put back the 16 pages of `58AD0000` (a block FMOD's thread and the pool's
     threads both write) taken that way, the ghost pass's second restore wrote their old copy over them, and the
     heap's free lists no longer agreed (a block linking back to the wrong one); a job worker then freed a block
     twice and a pool thread crashed in the heap. With the frame's close and the restore 5 ms apart (such writes
     then come in between far more often) the old build crashed in the rendering thread at `sub_82820828+0x402`,
     the guest stack of a player's 0.5.2 crash report (`82670F50 829674C0 82968AE0 ... 8296CF98 827410CC`), after
     the simulation had begun reallocating blocks of the engine's heap. A restore now drops the pages it put back
     from the pending log (`net/rollback.cpp`). Netplay between two PCs never goes back to the same frame twice (it
     goes back to the first frame whose guessed input was wrong, and checks every frame's input once): this took
     the ghost pass, or more than two players.
  2. A pool thread signalled just before the hold took its signal but counted as waiting until it ran on, and the
     hold looked at the signals only, so it could end while the thread ran its work (a skinned mesh's bone
     matrices, `sub_82917F38`; a PhysX step) through the frame's close and the restore, in two-PC netplay too. The
     sync test saw pool threads wake during a hold 2 to 5 times in 340 rollbacks; with 2 ms between a pool thread's
     wake and its going on (so that the race comes often), the plain sync test (one restore a rollback, as in
     netplay) had pool threads write the simulation's memory while it was held and crashed at its first rollback,
     twice (`sub_826B8CF8+0x13a` and near `sub_82BE4910`, null+4 and null+0x18, on pool threads). The hold now also
     waits until no pool thread has work given (DoWork sets it at +16 before it signals); work given once the hold
     is complete waits for its end and is the next frame's, which threads had such work at each kept frame's top
     goes back with it, and a thread that finds its work taken back by a restore waits again (`engine.cpp`). A first
     try that kept a woken pool thread waiting from the hold's start hung the game: the rendering thread can wait in
     a command for pool work (`sub_827EF9F8`) while the hold waits for the rendering thread.
  3. A restore put a page with volatile bytes back whole and then those bytes from a copy of the page taken just
     before, so that what another thread wrote to them in between was undone (older than the two faults above). The
     two-game mash test of the fixes above (lagged, both players mashing specials) froze the game in 2 of 18 runs:
     the GPU's interrupt (`sub_82BFB838`, which the SDK runs holding its global lock) spun for good on the D3D
     device's spinlock (`0x40016BE0`, the device's `+0x4260`, taken in `sub_82BF66D0`), whose word named the GPU's
     vertical blank thread as its owner while that thread was itself waiting for the global lock, and the main
     thread waited for that lock at the next frame's top (`EventOf`, the SDK's object table). Found with a build
     that writes every thread's host stack into hang.txt, and the spinlock's word read from the frozen game: the
     device's bytes are volatile, and a restore had copied its page while an interrupt held the spinlock and put
     that back after the interrupt let go. A restore now copies only a page's other bytes (`net/rollback.cpp`), so
     that no restore writes a volatile byte (every critical section outside the simulation heap is one too: a
     restore could have undone another thread's release of one the same way). With the old restore made to copy
     the device's page only while the spinlock was held and to wait 50 us before putting its volatile bytes back (a
     diagnostic build), the mash test froze the same way at its 33rd and at its 6th rollback, the spinlock's word
     the vertical blank thread's both times; the fixed restore under the same timing went through two whole fights
     (232 and 273 rollbacks in their busier game) without.
  The comparison also found UE3's GPU frame start time (`0x833F351C`, written by the GPU's interrupt thread beside
  the flag at `0x833F3520` that restores left alone) put back under that thread in a quarter of the ghost passes:
  volatile now. Not fixed: bytes just past the D3D device's 0x6000 (`0x4001898A`, 90 bytes) and beside the engine
  heap registry's lock (`0x8342EB5A`) that other threads write and restores put back; and a hold that times out
  (200 ms) still lets the frame's close and its restore go on under threads that run. With the pool's threads
  slowed 2 ms a wake the game fell to 9 frames a second, a hold timed out with the rendering thread in a D3D call,
  and the restore under it crashed it at `sub_82820828+0x402` too: the players' crash there may be such a hold.
  Tested, a private build of these sources with the ghost pass and the heap checks: the ghost sync test that crashed
  (Reptile and Sub-Zero, the same inputs) three times to 640 rollbacks with no block freed twice, the heap right
  after every restore and every frame run again, no crash; with the frame's close and the restore 5 ms apart, 340
  rollbacks the same; with the pool's race widened, the plain sync test went 160 rollbacks with no pool thread
  writing once a hold was complete, until the timed-out hold above. The lagged two-game test (50 ms, 30 of
  jitter, both attacking) twice with the build of 1. and 2.: both games to frame 9600 with no DESYNC, 165 to 173
  checks a game and none different, the fights' scripts the same frame for frame (0 of 4,935 and 0 of 5,028 frames
  differ), every input change read on its frame, 24 to 35 rollbacks every 10 s. With 3. as well, the build as
  committed: the ghost sync test twice more to 640 rollbacks with no block freed twice and no crash, and the lagged
  two-game test again (0 of 5,027 fight frames differ, no check different, every input change read on its frame);
  with hang.txt's every-thread stacks added, 17 fights of the mash test (seed 11, both games to frame 9600) with no
  freeze.
- **Rollback crashed the game in XAudio2's mixing thread** in 0.5.1 and 0.5.2 (found in players' crash reports
  and fixed 2026-10-29): `C0000005` at `sub_832C93D0+0x1f9` reading guest address 0, on the thread XAudio2 mixes
  on (`sub_832C92D0`), a minute or two into lagged online matches (four reports, three in a row from one player
  at 65 ms). Each pass the mixer takes the pointer to its statistics out of its engine object (`+656` of the object
  at `0x40083D40`, exchanged with 0), reads them and puts the pointer back. That object holds critical sections,
  and every critical section outside the simulation heap made its page logged whoever wrote it first: a frame
  whose first write of the page was the mixer putting its pointer back kept the page with the 0 in it, and a
  rollback past that frame put all of the page but the locks back under the mixer, which then read through the
  0. A sync test with `MK9_RB_TRACE_SHARED=1` listed eight of XAudio2's heap pages logged that way, written first
  by its mixing thread or by FMOD's output thread through XAudio2, and two pages of its globals in the
  executable's data. Since 0.5.3 a lock makes its page logged only in the executable's data (`rb::AddLock`,
  `LockForcesPage`): a page elsewhere with only locks on it is foreign when another thread writes it first,
  like any other. XAudio2's globals are volatile (`engine.cpp`, `kOtherThreadsGlobals`: its lock and its seven
  objects from `0x833C3B20`, its allocator's state at `0x834C19B0` and the rest of its block up to `0x834C2424`;
  only XAudio2's code addresses them). The same sync test then logged none of XAudio2's heap pages for its
  threads (14 pages logged for other threads' writes, from 24: the D3D device, the GPU callback's and the
  engine heap's globals, the executable's pages with locks on them), no simulation thread wrote a page another
  thread had written first (`MK9_RB_TRACE_FOREIGN=1`), and it went back 300 times without a crash; the
  lagged two-game test's fight stayed in step (0 checks and 0 of 4,963 frames different, 32-36 rollbacks
  every 10 s).
- **Every lobby match's result was player 2 winning 0-0** until 0.5.3 (found in the server's results on 2026-10-29 and
  fixed then): the game's count of the rounds each player won (`0x833C7A38`, `0x833C7A3C`) counts in a match played on
  one PC and in the tests' launcher-link matches (both reported `player 1 won 2-0`), but stayed 0 in every lobby
  match (crash reports in round 2 read 0-0), and the autopilot took the higher count as the winner, player 2 at a
  tie. So both games reported the same wrong result, the server took it as agreed, and the win went to player 2.
  Found by playing a real match again from a spectator's net log (`tools\feedreplay.py`; curzeta and Sethblazed01,
  Liu Kang winning both rounds) with dumps of the executable's data every 90 frames, beside a one-PC match each player won 2-0:
  `0x833C6C74` holds each round's winner in all three. The autopilot now counts the rounds from it as the round
  number goes on and counts the last round at CONTINUE? (`autopilot.cpp`); the replayed match reports `player 1
  won 2-0`.
- **Watching a match under way never started** until 0.5.3 (found 2026-10-29 in a player's own log and fixed then):
  the spectator's game stood at `meeting for match 1` until the player closed it (or gave up after 60 s). Its feed
  starts at the match's start, and a match under way has its next barrier in it too (the fight's, match 2): the
  next match's READY took the place of this one's in the barrier (`ready_match`), so the first barrier never had
  everyone. With that held until the barrier is over (`next_match`, `netplay.cpp`), a second fault showed: after
  a barrier a spectator counted only the first `delay` frames as held and waited for the rest to come, while the
  feed had brought them all already, and lost the players 10 s later ("stopped answering at frame 2"). Both fixed;
  tested by playing a real lobby match (curzeta and Sethblazed01, 2026-10-29) from a spectator's net log through
  a stand-in feed sent all at once (`tools\feedreplay.py`): menus, fight and CONTINUE? on the same frames as
  the spectator that watched it live (within one frame), all 260 checks equal.
- A crash before the first frame, seen once in about 130 test boots (2026-10-28, three games starting at the
  same moment) and in none of 107 more that tried to bring it back (2026-10-29, three or four at once with the same
  settings). `main()` (`sub_823669D8`) stored through r31 = 0 right after engine pre-init (`sub_82576640`)
  returned: the slots where pre-init had saved main's r30 and r31 and the address to go back to
  (0x7018F8E8-0x7018F8FB, just under main's frame) held zeros when it put them back, and the job workers it had
  just started read a null manager from their object (`worker+16`, in the simulation heap). So zeros landed on
  the main thread's stack and on a heap block while pre-init ran. Not the GPU (it writes physical memory, a view
  of its own), the stacks' fill (0xBE, not 0), APCs or the GPU interrupt (they run on contexts of their own) or the
  file read patch (it writes back what it read); what did is open. `crash.dmp` now holds the crashed guest
  thread's stack, so a next one shows those bytes.
  Found on the way, harmless as things lie: pre-init appends the command line (`"default.xex"`) to a stack buffer it
  never clears (`r31+240`). New stacks are zero on the console but 0xBE in the SDK, so the text lands after the first
  zero byte above: the top of the performance frequency read just before, over 6 bytes of a saved r30 that main
  never reads again.
- Another stop before the first frame, at a file read: four games booting at once (2026-11-03, two two-game tests
  started together), one crashed in the SDK's read (`rexruntime.dll+0x3ff086` reading 0x18, from `sub_827288E0+0xf3`
  under `sub_82728A70`, 1 s in) and another stood still at the same moment of its boot, its main thread logging
  nothing more. A player's crash report of 0.5.2 (Jenogi, 2 s in) has the same place. Not looked into yet. One
  diagnostic build of the restore fix above (2026-11-03) stopped there in 10 of 15 boots, alone as well, while
  builds that differ from it only in code a restore runs booted every time but once: the stop depends on how the
  executable is laid out or timed, and such a build brings it on at will.
- Four-player TWO VS TWO (slots 2-3) is in the netplay core but untested.
- The autopilot steers menus by fixed steps from a fresh save's cursor; the roster grid is not
  mapped yet, so the launcher cannot pick a named fighter (players pick in the game).
- **A fighter striped and see-through from a fight's first hits** in every build before 2026-10-29, and in Xenia
  Canary (found and fixed 2026-10-29, see "Blood and damage textures"): draws dropped while their pipelines were
  being created let a resolve copy the last frame's image into a blood and damage texture the game draws from
  itself. None of the settings tried first changed it (`render_target_path_d3d12=rov`, `readback_resolve=full`,
  `native_2x_msaa=false`, `gpu_allow_invalid_fetch_constants=true`,
  `texture_cache_memory_limit_render_to_texture=256`, `MK9_SIMHEAP=0`): none of them stops draws being dropped.
