# MK9 Online

Mortal Kombat Komplete Edition (Xbox 360) running natively on Windows PCs, with online play:
Kasual and Ranked matchmaking, lobbies with spectators and King of the Hill, single player, and
controls you remap in the launcher.

The game's code is not in this repository and never will be. Like the Tekken 6 and MK3D projects,
every player builds their own copy on their own PC from **their own disc**: the launcher reads your
Xbox 360 disc image, translates the game's PowerPC code to C++ with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and compiles it with clang.

## Playing

1. Install [Build Tools for Visual Studio](https://visualstudio.microsoft.com/downloads/) with the
   **Desktop development with C++** workload (the compiler's libraries; free).
2. Download `MK9Online-<version>.zip` from [Releases](../../releases) and unpack it somewhere you can
   write to (Documents, say), in a folder whose path has no `( ) & ^ %`: the compiler's tools cannot
   build there (a second download's `MK9Online-0.3.2 (1)` is one; rename it first).
3. Start `MK9Launcher.exe`, open **Build game**, pick your Mortal Kombat Komplete Edition disc image
   (`.iso`) and press **Extract and build**. The first build takes about 15 minutes.
4. Sign in (top right) and play: **Kasual** or **Ranked** (One vs One, Two vs Two, Test Your Luck),
   **Lobbies**, or **Single player** for the game's own modes offline.

The launcher updates itself from this repository's releases; when an update changes the game, it
builds the game again on its own. If a build fails, the Build game page says why, and everything the
build printed is in `games\mk9\build.log`.

If the game crashes, the launcher keeps a crash report in `crashes\` beside it and offers to send it to
the MK9 Online server, so whoever runs it can see what went wrong.

## What works and what does not (yet)

- Online play uses rollback netcode (the default; lockstep is the fallback in Settings) with an
  input delay set from the ping. How it works and how it was tested: [docs/GAME.md](docs/GAME.md#rollback).
- The game runs at 60 frames a second at most, as on the Xbox 360. On a laptop with two graphics chips it
  uses the faster one.
- Two players who cannot reach each other play through the server; a player whose network blocks UDP
  altogether plays through their connection to the server (a little more lag, but the match goes ahead).
- Tested: One vs One, Two vs Two (two players), Test Your Luck, results, rematches, spectators.
  Four-player Two vs Two is not tested yet.
- The Xbox 360 GPU emulation draws a striped texture on fighters after hits.

## Building from source

- The game: [docs/GAME.md](docs/GAME.md) (`mk9\build.cmd release`; needs the extracted disc in
  `game\disc`, LLVM in `external\llvm`, the SDK in `external\rexglue-sdk`)
- The launcher and the server: [docs/LAUNCHER.md](docs/LAUNCHER.md)
- A release: `tools\release.ps1`

Not affiliated with Warner Bros., NetherRealm Studios or Microsoft. Mortal Kombat is a trademark
of Warner Bros. Entertainment Inc. You need your own legally obtained copy of the game.
