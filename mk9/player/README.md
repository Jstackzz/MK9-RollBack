# What a player's PC needs to build the game: nothing but the package

Nobody ships the game's code: every PC translates its own copy from its own disc, and that translated C++ has to be
compiled there. Until 0.5.7 that took Visual Studio Build Tools (several GB) for Microsoft's C++ headers and libraries.
Now the zip holds everything, and the build works on a PC with no developer tools at all.

## How

The game is two parts:

| part | what | built |
|---|---|---|
| `games\mk9\host\mk9.exe` | everything that is ours: hooks, netplay, rollback, the app, linked with the C++ runtime | at release, on the developer's PC (`tools\build-player-host.cmd` -> `dist\host`) |
| `mk9game.dll` | only the game's translated code | on the player's PC, by `build-game.cmd` |

`mk9game.dll` needs no C++ library, no Windows headers and no CRT. It is compiled against

- `include\` here: tiny stand-ins for the C++ library's headers and for the SDK headers that drag in host code. They hold
  only what the translated code uses (put first on the include path, with `-nostdlibinc`);
- the SDK's own `rex/ppc/*` headers and simde;
- clang's own headers (intrinsics, `stdint.h`);

and linked (`/NODEFAULTLIB /NOENTRY`) against import libraries that are all made here: `mk9.lib` (the hooks, from
`mk9.exe`), `rexruntime.lib` (the SDK's), and three tiny ones from the `.def` files here for the few C runtime functions
the translated code calls.

The two parts call each other: `mk9.exe` calls the original game functions it hooks (exported by the DLL, listed by
`tools\player\boundary.py` from `mk9.exe`'s own objects), and the DLL calls the hooks (exported by `mk9.exe`). Windows
resolves a DLL's imports from the exe that is already loaded, as long as the exe keeps its name.

**A hook is only a weak-symbol override**: the translated code defines each game function as a weak alias of the
original, and a weak name does not pull a member out of a library. `build.ninja` gives the linker `mk9.lib` with
`/WHOLEARCHIVE`. Without it the DLL still links and runs, with every hook silently left out: after any change here check
that `mk9game.dll` imports all of them (`llvm-readobj --coff-imports mk9game.dll`: 43 `sub_*` from `mk9.exe` today).

## Why this is the same game

The translated code's machine code was compared function by function with a normal build's (all 98,482 functions):
identical except for

- the 658 functions with a guest lock (`mfmsr`/`mtmsrd`): no exception cleanup, and the unreachable "mutex error" paths
  of Microsoft's `std::recursive_mutex` are gone;
- full memory barriers: `lock or [rsp]` instead of `lock inc [local]`;
- guest traps, which only log: their logging is now a call into `mk9.exe`.

When the SDK or the codegen changes, redo this (`tools\player\compare_objs.py`), and when a new C runtime function shows
up the link on the player's PC says which: add it to the `.def`.

## Checking a package

- `tools\player\test-clean-build.ps1 -Game <package>\games\mk9` builds it as a PC with nothing installed would; add
  `-March x86-64-v2` for what a CPU without AVX2 builds (the libcalls differ).
- `tools\player\check_package.py <package>` fails if anything needs a DLL outside the package and Windows, or if a built
  `mk9game.dll` left a hook out.
- `Start-LinkLocal -Bin <new> -BinB <old>` plays one build against another of the same release.

## Folders

Every path in the compile is relative, and no `cmd.exe` is involved, so the folder can have `( ) & ^ %` in its name.
`package.ps1` stamps `mk9.exe` and `rexglue.exe` with `utf8.manifest`: both keep paths in narrow strings, so without it a
folder with an accented or non-Latin letter (a Windows account's name, say) stops codegen and the game at once.

## Updates

`build-game.cmd` costs seconds when nothing changed: codegen sees its inputs are the same, ninja has no work, and the new
`mk9.exe` is copied in. A release that changes only `mk9.exe`'s code does not make anyone compile the game; a new hook
is a relink. Only a change to the codegen's config or to the headers here compiles everything again.
