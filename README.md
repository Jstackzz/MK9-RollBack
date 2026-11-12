# MK9 Online

Mortal Kombat Komplete Edition (Xbox 360) running natively on Windows PCs, with online play:
Kasual and Ranked matchmaking, lobbies with spectators and King of the Hill, single player, and
controls you remap in the launcher.

The game's code is not in this repository and never will be. Like the Tekken 6 and MK3D projects,
every player builds their own copy on their own PC from **their own disc**: the launcher reads your
Xbox 360 disc image, translates the game's PowerPC code to C++ with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and compiles it with clang.

## Playing

There is nothing to install and nothing else to download: the zip has everything.

1. Download `MK9Online-<version>.zip` from [Releases](../../releases) and unpack it somewhere you can
   write to (Documents, say).
2. Start `MK9Launcher.exe`. It opens on **Build game**: pick your Mortal Kombat Komplete Edition disc image
   (`.iso`; it finds it for you if it is beside MK9 Online or in your Downloads, Desktop or Documents) and
   press **Extract and build**. That happens once and takes a few minutes.
3. Sign in (top right) and play: **Kasual** or **Ranked** (One vs One, Two vs Two, Test Your Luck),
   **Lobbies**, or **Single player** for the game's own modes offline.

The launcher updates itself from this repository's releases; when an update changes the game, it
puts the game together again on its own (usually in seconds). Until that is done it does not search
for matches or ready up in a lobby, so nobody gets matched with a game from another release. If a
build fails, the Build game page says why, and everything the build printed is in `games\mk9\build.log`.

The game skips its intro movies and title screen: it starts at the main menu in a few seconds.

If the game crashes, the launcher keeps a crash report in `crashes\` beside it and offers to send it to
the MK9 Online server, so whoever runs it can see what went wrong.

If something is drawn wrong (flickering textures, a fighter or floor in the wrong colours, missing health
bars), press **F3** while it is on the screen. The game saves a picture of it and of the next 20 frames and
says so at the top of the screen; the launcher then sends them to the developers by itself, with the end of
the game's log (no IP addresses, no folder names), as long as you are signed in.

## What works and what does not (yet)

- Online play uses rollback netcode (the default; lockstep is the fallback in Settings) with an
  input delay set from the ping. How it works and how it was tested: [docs/GAME.md](docs/GAME.md#rollback).
- The game runs at 60 frames a second at most, as on the Xbox 360. On a laptop with two graphics chips it
  uses the faster one.
- Settings, Display: V-sync (on by default, so the picture never tears; off for a G-SYNC or FreeSync monitor), the
  game window and its size, and the resolution the game draws at (1280x720 as on the Xbox 360, up to 3840x2160).
- Two players who cannot reach each other play through the server; a player whose network blocks UDP
  altogether plays through their connection to the server (a little more lag, but the match goes ahead).
- Tested: One vs One, Two vs Two (two players), Test Your Luck, results, rematches, spectators.
  Four-player Two vs Two is not tested yet.
- In an online match, and one you watch, each player's name stands above their health bar.
- Most crash reports sent from 0.4.0 were the game running out of its 64 effects (sparks, blood, fire) in
  busy scenes; since 0.5.0 the oldest effect makes room instead. Players named Cyrax's air grab, Noob
  Saibot's teleport slam and the Church among the crashes: send the report if one still crashes.
- Until 0.5.0 a fighter could turn striped and see-through from a fight's first hits (in the Xenia
  emulator too): the GPU emulation copied the last frame into the fighter's blood and damage texture while
  the shaders for it were still being made. Fixed in 0.5.0 (docs/GAME.md, "Blood and damage textures").
- Until 0.5.1 a game waiting for a match did not stay at its menu: after a minute MK9 went back to its title
  and then played its demo fight, and a lobby match started after such a wait desynced or lost the other
  player. And every release presented the window about 300 times a second (frame counters read it as
  uncapped; the game itself stayed at 60): the SDK's achievement toast overlay, open the whole time, kept
  it repainting. Both fixed in 0.5.1 (docs/GAME.md, "The launcher link", "Frame rate").
- Until 0.5.1 every rollback held the game up: the GPU emulation drew the frames a rollback runs again in
  full, and each of them waited for the display's 60 Hz blank, so a lagged fight slowed down where it rolled
  back (to about 53 frames a second, with rollbacks of up to 120 ms) and felt muddy. In 0.5.1 those frames
  draw only the fighters' and the floor's blood and wait for no blank: less than half the cost a frame,
  rollbacks of 40 ms at most on the same test (docs/GAME.md, "What a rollback costs").
- Until 0.5.1 the game crashed when a rollback went back into a fight's first frames (someone pressing buttons
  as a fight starts, with some lag): the other player saw it stop answering. And from a rematch on, rollbacks
  did not put back the key presses and PhysX's events they keep. Fixed in 0.5.1 (docs/GAME.md, "Known gaps").
- Until 0.5.2 nearly every online match went out of step: each graphics card answered the game's occlusion
  queries (how much of something got drawn) in its own way, and some of the game's effects follow those
  answers, so two players' games drew differently and the desync check stopped the match. Since 0.5.2 every PC
  gets the same answer (docs/GAME.md, "Netplay").
- Until 0.5.3 a lagged online match could crash a minute or two in, in the game's sound mixer (an access
  violation in XAudio2's code): a rollback put part of the mixer's memory back under it while it ran. Fixed in
  0.5.3 (docs/GAME.md, "Known gaps").
- Until 0.5.3 watching a match already under way never started: the spectator's game waited at the match's
  start until it gave up. Fixed in 0.5.3.
- Until 0.5.3 every lobby match's result came out as player 2 winning 0-0, whoever won: the game's own count of
  the rounds won stays 0 in online matches. Since 0.5.3 the game counts the rounds from each round's winner.
- Until 0.5.3 a rollback could stop a match as out of step when both games were alike: once it took back the
  first random numbers a part of the game had drawn, its game kept counting that part (at 0) and the other
  player's did not, so their checks differed. Fixed in 0.5.3 (docs/GAME.md, "Known gaps").
- Until 0.5.4 a Kasual or ranked rematch played through the server (a player who hides their IP address, two
  players who cannot reach each other) closed both games and started new ones; since 0.5.4 it starts from the
  game's own REMATCH or SELECT A NEW FIGHTER at its CONTINUE? menu, as a direct one did. The input delay rises
  sooner with the ping (4 frames at 180 ms, as MK3D Online), and a hidden match over 100 ms through the server
  tries a direct connection once (Settings keeps it hidden for good when you ask).
- Since 0.5.4, as in MK3D Online: a ranked set goes to the first to win 3 matches (was 2), every ranked rating has
  its rank title (Kombatant at the start, up to Elder God of Destruction), the launcher searches again by itself when
  a set or a Kasual series ends (a box on the Ranked and Kasual pages turns it off), and there is no mouse pointer
  over the game.
- Until 0.5.5 fights could go out of step when the two PCs' loading differed: by the objects each game made while
  loading (a fight stopped at its first check), and by a frame count that started with the loading, which made the
  two games run the fighters' scripts in opposite orders (busy fights went out of step a few seconds after a hit
  that sparked effects from both fighters at once). Since 0.5.5 both are counted from the fight's start alike, and
  the menus are no longer compared (docs/GAME.md, "Known gaps").
- Until 0.5.5 a rollback could crash the game in its drawing or freeze a fight for good: it could bring back memory
  of the frames it took back, run on while a helper thread worked, or undo what another thread had just written.
  Fixed in 0.5.5 (docs/GAME.md, "Known gaps"). Since 0.5.5, as in MK3D Online, the launcher also tells the server in
  a few lines where the game crashed or stood still in a match; SEND REPORT still sends the whole report.
- Until 0.5.6 a rollback could play a sound again, or cut it and start it over; since 0.5.6 what you already heard is left
  alone (docs/GAME.md, "Sounds"). A match now stops as out of step only when the fight itself differs (timer, round,
  health or meters), and fights start on the same frame on both PCs: a few milliseconds' stall while a fight began could
  put one game a frame behind for the whole fight (docs/GAME.md, "Known gaps").
- Until 0.5.6 rollbacks could crash the game in the health bars, in its memory bookkeeping and as a match ended, and a
  Radeon could lose the card in rollback fights. Fixed in 0.5.6 (docs/GAME.md, "Known gaps").
- Until 0.5.7 you had to install Visual Studio Build Tools (several GB) before the game could be built, and the folder
  could not have `( ) & ^ %` in its name. Since 0.5.7 the zip has everything and any folder works: only the game's own
  code is compiled on your PC, which also takes a few minutes instead of 15 (mk9/player/README.md).
- Until 0.5.9 the input delay the server chose for a rollback match was a frame short of the one-way trip (5 frames
  at 193 ms of ping, 2 at 82), so nearly every input of the other player came a frame late and every change of it
  was a rollback, each of which holds the game up a frame or more: the server's match log rated every 0.5.9 match
  "stutter" on both PCs. Since 0.5.10 the delay covers the trip with half a frame to spare (3 from 51 ms of ping, 4
  from 84, 7 at 193, 8 at most): more frames between a press and the move, far fewer frames shown twice. Since 0.5.11
  the setting is back at 2 frames by default, as before 0.5.9; Auto stays one choice away in Settings for a longer
  ping, where 2 means a rollback for every input of the other player that comes late (docs/PROTOCOL.md, "Input delay").
- Until 0.5.11 a lobby's game closed after every match and the next match booted a new one on both PCs: 9.5 seconds to
  the main menu and 10 more for the game to walk its menus to the select screen. Since 0.5.11 (with the 0.3.6 server)
  a lobby match with a result leaves both games open, back at their main menus, and when the same two ready up again
  the next match starts in them: the select screen comes about 10 seconds after the countdown, not 20. If one of them
  leaves, goes to watch or is replaced, their games close and the next match starts new ones on both PCs (docs/LAUNCHER.md,
  "Lobby").
- Until 0.5.9 a rollback could crash the game in its HUD a few minutes into a fight (the UI library's heap lost the
  page table of memory the rollback brought back). Fixed in 0.5.10 (docs/GAME.md, "Known gaps"). A game that stands
  still in a match now reports every thread, for the select-screen freeze two players met.
- Until 0.5.10 one PC's hiccup made the other stutter: a frame that came late (a rollback that overran, a stall) was
  never made up, so that PC was a frame or two behind for the rest of the match and the other held frames back to wait
  for it. More than half of the frames shown twice in 0.5.10's first day of matches were those holds, on the PC that
  had no hiccup. Since 0.5.11 a PC that came late makes up to a frame of it by starting its next frames close
  together (its own picture then runs up to a frame behind), and a PC ahead holds for a lead only once it has lasted
  a quarter of a second. In test fights on one PC, the other PC's screen showed 21 gaps over 25 ms instead of 37 when
  one PC stalled 40 ms every 4 s, 2 instead of 31 for stalls of 22 ms every 1.5 s, and 7 instead of 45 in a match with
  40 ms of jitter. The server's match log counts what the screen got, not the frames' own starts (docs/PROTOCOL.md,
  "Match stats"), and says what made late frames late.
- Until 0.5.10 the game wrote two log lines per GPU interrupt (75 a second, under the lock every kernel call of the
  game's threads takes) and a few a frame in some fights, each a flushed write to disk: a PC with a slow disk or a busy
  virus scanner could stall on them (not measured on such a PC). Repeated warnings are now written 8 times and then rarely. The game also asks Windows
  not to throttle it or its timers when its window is behind another (OBS, remote play) or the PC saves battery, and
  its pipeline threads yield to the game's own (docs/GAME.md, "Frame rate").

## Building from source

- The game: [docs/GAME.md](docs/GAME.md) (`mk9\build.cmd release`; needs the extracted disc in
  `game\disc`, LLVM in `external\llvm`, the SDK in `external\rexglue-sdk`)
- The launcher and the server: [docs/LAUNCHER.md](docs/LAUNCHER.md)
- A release: `tools\release.ps1`

Not affiliated with Warner Bros., NetherRealm Studios or Microsoft. Mortal Kombat is a trademark
of Warner Bros. Entertainment Inc. You need your own legally obtained copy of the game.
