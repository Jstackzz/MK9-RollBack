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
   write to (Documents, say).
3. Start `MK9Launcher.exe`, open **Build game**, pick your Mortal Kombat Komplete Edition disc image
   (`.iso`) and press **Extract and build**. The first build takes about 15 minutes.
4. Sign in (top right) and play: **Kasual** or **Ranked** (One vs One, Two vs Two, Test Your Luck),
   **Lobbies**, or **Single player** for the game's own modes offline.

The launcher updates itself from this repository's releases; when an update changes the game, it
builds the game again on its own.

## What works and what does not (yet)

- Online play uses lockstep netcode with an input delay set from the ping. Rollback netcode is
  in progress: see [docs/GAME.md](docs/GAME.md#rollback-where-it-stands).
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
