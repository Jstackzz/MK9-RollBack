# Assembles the player package in dist\MK9-Online (no game data, no game code: each player builds
# their own copy from their own disc, see tools\player\build-game.cmd). Nothing else has to be installed
# on the player's PC: mk9\player\README.md says how.
#   MK9Launcher.exe + Qt          the launcher (build\launcher, after launcher\build.cmd)
#   Microsoft's C++ runtime DLLs  beside the launcher and in games\mk9\sdk\bin, for the PCs without them
#   toolchain\                    clang, lld-link, ninja and clang's own headers
#   games\mk9\build-game.cmd      codegen + compile on the player's PC; build.ninja is the compile
#   games\mk9\host\mk9.exe        everything but the game's code (dist\host, after tools\build-player-host.cmd)
#   games\mk9\project\            what codegen reads: the manifest and config
#   games\mk9\player\             the headers and import libraries the game's code compiles and links with
#   games\mk9\sdk\                the prebuilt ReXGlue runtime and codegen tool (from dist\sdk)
#   docs\                         PLAYING.md, GAME.md
#   defaults.ini                  the server the launcher uses until a player picks another: dist\defaults.ini
#                                 (not in the repository: it names the machine), [server] address= and fingerprint=
# The folder is named MK9-Online with VERSION.txt ("MK9-Online <version>, packaged <date>"): what the
# launcher's updater checks in a release zip (launcher/src/Updater.cpp).
# Usage: . tools\package.ps1; New-PlayerPackage [-Out dist\MK9-Online] [-NoLauncher]
function New-PlayerPackage {
    param([string]$Out = "", [switch]$NoLauncher)
    $root = Split-Path $PSScriptRoot -Parent
    if (-not $root) { $root = "C:\Users\Owner\source\repos\mk9-rollback" }
    if (-not $Out) { $Out = Join-Path $root "dist\MK9-Online" }
    $version = (Get-Content (Join-Path $root "VERSION") -TotalCount 1).Trim()
    $sdk = Join-Path $root "dist\sdk"
    if (-not (Test-Path (Join-Path $sdk "bin\rexglue.exe"))) { throw "no SDK in ${sdk}: build and install it first (see docs\GAME.md)" }
    New-Item -ItemType Directory -Force $Out | Out-Null
    $game = Join-Path $Out "games\mk9"
    $proj = Join-Path $game "project"
    New-Item -ItemType Directory -Force $proj | Out-Null

    # the game's project: what codegen reads, with the manifest pointing at games\mk9\game (where the disc is extracted)
    Copy-Item -Force (Join-Path $root "mk9\VERSION") $proj  # the game's release, not the launcher's (mk9\CMakeLists.txt)
    robocopy (Join-Path $root "mk9\config") (Join-Path $proj "config") /MIR /NFL /NDL /NJH /NJS | Out-Null
    $manifest = Get-Content (Join-Path $root "mk9\mk9_manifest.toml") -Raw
    $manifest = $manifest.Replace('game_root = "../game/disc"', 'game_root = "../game"').Replace('file_path = "../game/disc/default.xex"', 'file_path = "../game/default.xex"')
    [System.IO.File]::WriteAllText((Join-Path $proj "mk9_manifest.toml"), $manifest)
    # (with Windows line ends whatever the checkout has: cmd.exe can miss a label in a script without them)
    $script = (Get-Content -Raw (Join-Path $root "tools\player\build-game.cmd")) -replace "`r?`n", "`r`n"
    [System.IO.File]::WriteAllText((Join-Path $game "build-game.cmd"), $script, [System.Text.Encoding]::ASCII)

    # mk9.exe without the game's code, and what the game's code compiles and links with on the player's PC
    $hostBuild = Join-Path $root "dist\host"
    foreach ($f in "mk9.exe", "mk9.lib", "player\game_link.rsp") {
        if (-not (Test-Path (Join-Path $hostBuild $f))) { throw "no $f in ${hostBuild}: run tools\build-player-host.cmd first" }
    }
    $hostDir = Join-Path $game "host"
    New-Item -ItemType Directory -Force $hostDir | Out-Null
    Copy-Item -Force (Join-Path $hostBuild "mk9.exe") $hostDir
    $player = Join-Path $game "player"
    robocopy (Join-Path $root "mk9\player\include") (Join-Path $player "include") /MIR /NFL /NDL /NJH /NJS | Out-Null
    Copy-Item -Force (Join-Path $root "mk9\player\game_stub.cpp") $player
    Copy-Item -Force (Join-Path $hostBuild "player\game_link.rsp") $player
    $libs = Join-Path $player "lib"
    New-Item -ItemType Directory -Force $libs | Out-Null
    Copy-Item -Force (Join-Path $hostBuild "mk9.lib") $libs
    $llvm = Join-Path $root "external\llvm"
    foreach ($d in "msvcp140", "vcruntime140", "ucrtbase") {
        & (Join-Path $llvm "bin\llvm-lib.exe") /nologo /machine:x64 "/def:$(Join-Path $root "mk9\player\$d.def")" "/out:$(Join-Path $libs "$d.lib")"
        if ($LASTEXITCODE -ne 0) { throw "llvm-lib failed for $d" }
    }
    Remove-Item -Force (Join-Path $libs "*.exp") -ErrorAction SilentlyContinue
    python (Join-Path $root "tools\player\make_build_ninja.py") --sources (Join-Path $root "mk9\generated\default\sources.cmake") --out (Join-Path $game "build.ninja") | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "make_build_ninja failed" }

    # the prebuilt runtime and codegen tool, and the SDK headers the game's code compiles with (player\include stands in for the rest)
    $sdkTo = Join-Path $game "sdk"
    New-Item -ItemType Directory -Force (Join-Path $sdkTo "bin"), (Join-Path $sdkTo "lib") | Out-Null
    robocopy (Join-Path $sdk "bin") (Join-Path $sdkTo "bin") /MIR /NFL /NDL /NJH /NJS | Out-Null
    Copy-Item -Force (Join-Path $sdk "lib\rexruntime.lib") (Join-Path $sdkTo "lib")
    robocopy (Join-Path $sdk "include\simde") (Join-Path $sdkTo "include\simde") /MIR /NFL /NDL /NJH /NJS | Out-Null
    foreach ($h in "rex\platform.h", "rex\image_info.h", "rex\platform\fpscr.h", "rex\ppc\context.h", "rex\ppc\func.h", "rex\ppc\intrinsics.h") {
        $to = Join-Path $sdkTo "include\$h"
        New-Item -ItemType Directory -Force (Split-Path $to -Parent) | Out-Null
        Copy-Item -Force (Join-Path $sdk "include\$h") $to
    }

    # the game and the codegen tool keep paths in narrow strings: as UTF-8 programs they work from any folder, a Windows
    # account's name with an accent or another script in it too
    $mt = @(Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\mt.exe" | Sort-Object FullName)
    if ($mt.Count -eq 0) { throw "no mt.exe in the Windows SDK" }
    foreach ($exe in (Join-Path $hostDir "mk9.exe"), (Join-Path $sdkTo "bin\rexglue.exe")) {
        & $mt[-1].FullName -nologo -manifest (Join-Path $root "mk9\player\utf8.manifest") "-updateresource:$exe;#1"
        if ($LASTEXITCODE -ne 0) { throw "mt failed for $exe" }
    }

    # Microsoft's C++ runtime (redistributable with an application): the launcher, the codegen tool and the game need it,
    # and a PC that has never had it, or has an older one, would stop at "VCRUNTIME140.dll was not found"
    $redist = @(Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory | Sort-Object FullName)
    if ($redist.Count -eq 0) { throw "no Microsoft.VC*.CRT folder under Visual Studio's VC\Redist" }
    $crt = $redist[-1].FullName
    foreach ($d in "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll") {
        Copy-Item -Force (Join-Path $crt $d) (Join-Path $sdkTo "bin")
        Copy-Item -Force (Join-Path $crt $d) $Out
    }

    # clang, lld-link and clang's headers: what build.ninja needs of LLVM. Ninja runs it (the copy in Visual Studio's CMake:
    # Apache-2.0, its licence beside it)
    $tc = Join-Path $Out "toolchain"
    New-Item -ItemType Directory -Force (Join-Path $tc "bin") | Out-Null
    Copy-Item -Force (Join-Path $llvm "bin\clang.exe") (Join-Path $tc "bin\clang.exe")
    Copy-Item -Force (Join-Path $llvm "bin\lld-link.exe") (Join-Path $tc "bin\lld-link.exe")
    $ninja = @(Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" | Sort-Object FullName)
    if ($ninja.Count -eq 0) { throw "no ninja.exe in Visual Studio's CMake" }
    Copy-Item -Force $ninja[-1].FullName (Join-Path $tc "bin\ninja.exe")
    $clangVer = (Get-ChildItem (Join-Path $llvm "lib\clang") -Directory | Select-Object -First 1).Name
    robocopy (Join-Path $llvm "lib\clang\$clangVer\include") (Join-Path $tc "lib\clang\$clangVer\include") /MIR /NFL /NDL /NJH /NJS | Out-Null
    Copy-Item -Force (Join-Path $llvm "LICENSE.TXT") (Join-Path $tc "LICENSE.TXT") -ErrorAction SilentlyContinue
    Copy-Item -Force (Join-Path $root "tools\player\NINJA-LICENSE.txt") (Join-Path $tc "NINJA-LICENSE.txt")

    # the server: dist\defaults.ini, kept out of the repository
    $defaults = Join-Path $root "dist\defaults.ini"
    if (Test-Path $defaults) {
        Copy-Item -Force $defaults (Join-Path $Out "defaults.ini")
    } else {
        Write-Host "no dist\defaults.ini: the package's launcher looks for a server on the player's own PC (127.0.0.1:7777)"
    }

    # the launcher (windeployqt already put Qt beside it)
    if (-not $NoLauncher) {
        $lb = Join-Path $root "build\launcher"
        if (Test-Path (Join-Path $lb "MK9Launcher.exe")) {
            # the build's own files stay behind (moc output, the static SDL3's build folder, the compile commands),
            # and windeployqt's installer of the runtime the package has as DLLs
            robocopy $lb $Out /E /XD CMakeFiles _deps .qt MK9Launcher_autogen sdl3 /XF *.ninja* CMakeCache.txt cmake_install.cmake compile_commands.json *.obj *.pdb *.ilk *.exp *.lib vc_redist.x64.exe /NFL /NDL /NJH /NJS | Out-Null
        } else {
            Write-Host "no launcher in $lb (build it with launcher\build.cmd); packaging without it"
        }
    }
    New-Item -ItemType Directory -Force (Join-Path $Out "docs") | Out-Null
    foreach ($d in "PLAYING.md", "GAME.md", "LAUNCHER.md") {
        $p = Join-Path $root "docs\$d"
        if (Test-Path $p) { Copy-Item -Force $p (Join-Path $Out "docs") }
    }
    [System.IO.File]::WriteAllText((Join-Path $Out "VERSION.txt"),
        "MK9-Online $version, packaged $(Get-Date -Format 'yyyy-MM-dd HH:mm')`r`n")
    $size = (Get-ChildItem $Out -Recurse -File | Measure-Object -Property Length -Sum).Sum
    "package in $Out ({0:N0} MB)" -f ($size / 1MB)
}
