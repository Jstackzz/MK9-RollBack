# Assembles the player package in dist\MK9-Online (no game data, no game code: each player builds
# their own copy from their own disc, see tools\player\build-game.cmd):
#   MK9Launcher.exe + Qt          the launcher (build\launcher, after launcher\build.cmd)
#   toolchain\                    clang, clang++, lld-link and clang's own headers from external\llvm
#   games\mk9\build-game.cmd      codegen + compile on the player's PC
#   games\mk9\project\            the game's project: CMake files, src, config, manifest
#   games\mk9\sdk\                the prebuilt ReXGlue runtime and codegen tool (dist\sdk)
#   docs\                         PLAYING.md, GAME.md
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

    # the game's project, with the manifest pointing at games\mk9\game (where the disc is extracted)
    foreach ($f in "CMakeLists.txt", "CMakePresets.json") { Copy-Item -Force (Join-Path $root "mk9\$f") $proj }
    Copy-Item -Force (Join-Path $root "VERSION") $proj
    robocopy (Join-Path $root "mk9\src") (Join-Path $proj "src") /MIR /NFL /NDL /NJH /NJS | Out-Null
    robocopy (Join-Path $root "mk9\config") (Join-Path $proj "config") /MIR /NFL /NDL /NJH /NJS | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $proj "generated") | Out-Null
    Copy-Item -Force (Join-Path $root "mk9\generated\rexglue.cmake") (Join-Path $proj "generated")
    $manifest = Get-Content (Join-Path $root "mk9\mk9_manifest.toml") -Raw
    $manifest = $manifest.Replace('game_root = "../game/disc"', 'game_root = "../game"').Replace('file_path = "../game/disc/default.xex"', 'file_path = "../game/default.xex"')
    [System.IO.File]::WriteAllText((Join-Path $proj "mk9_manifest.toml"), $manifest)
    Copy-Item -Force (Join-Path $root "tools\player\build-game.cmd") $game

    # the prebuilt runtime and codegen tool
    robocopy $sdk (Join-Path $game "sdk") /MIR /NFL /NDL /NJH /NJS | Out-Null

    # clang (as clang and clang++), lld-link and clang's headers: what build-game.cmd needs of LLVM
    $tc = Join-Path $Out "toolchain"
    New-Item -ItemType Directory -Force (Join-Path $tc "bin") | Out-Null
    $llvm = Join-Path $root "external\llvm"
    # (both are self-contained; build-game.cmd makes clang++.exe a hard link of clang.exe)
    Copy-Item -Force (Join-Path $llvm "bin\clang.exe") (Join-Path $tc "bin\clang.exe")
    Copy-Item -Force (Join-Path $llvm "bin\lld-link.exe") (Join-Path $tc "bin\lld-link.exe")
    Remove-Item -Force (Join-Path $tc "bin\*.dll"), (Join-Path $tc "bin\clang++.exe") -ErrorAction SilentlyContinue
    robocopy (Join-Path $llvm "lib\clang") (Join-Path $tc "lib\clang") /MIR /NFL /NDL /NJH /NJS | Out-Null
    Copy-Item -Force (Join-Path $llvm "LICENSE.TXT") (Join-Path $tc "LICENSE.TXT") -ErrorAction SilentlyContinue

    # the launcher (windeployqt already put Qt beside it)
    if (-not $NoLauncher) {
        $lb = Join-Path $root "build\launcher"
        if (Test-Path (Join-Path $lb "MK9Launcher.exe")) {
            robocopy $lb $Out /E /XD CMakeFiles _deps .qt /XF *.ninja* CMakeCache.txt cmake_install.cmake *.obj *.pdb *.ilk *.exp *.lib /NFL /NDL /NJH /NJS | Out-Null
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
