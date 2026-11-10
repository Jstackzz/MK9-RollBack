@echo off
rem Builds the native MK9 game on this PC from the player's own disc (the Tekken 6 / MK3D way: nobody ships the game's
rem code, every PC translates its own copy). Run by the launcher's Build game page once the disc is extracted into
rem game\ beside this script, or by hand. Nothing has to be installed: everything used here comes with the package
rem (mk9\player\README.md in the repository).
rem   1. rexglue codegen: game\default.xex -> project\generated (C++ for every function of the game)
rem   2. ninja + clang: the translated code -> build\mk9game.dll
rem   3. mk9.exe (ready made, without the game's code), mk9game.dll and the runtime's DLLs next to this script
rem Every path below is relative to this folder, so its name and place do not matter.
setlocal EnableExtensions
cd /d "%~dp0" || exit /b 1
set "TOOLS=..\..\toolchain\bin"
if not exist game\default.xex goto nogame
if not exist host\mk9.exe goto missing
if not exist "%TOOLS%\clang.exe" goto missing
if not exist "%TOOLS%\ninja.exe" goto missing

rem a build folder from before 0.5.7 (CMake's, with everything in it compiled the old way)
if exist build\CMakeCache.txt rmdir /s /q build
if not exist build mkdir build || exit /b 1

rem AVX2 PCs get the faster code, older ones the SSE4 build (clang knows this PC's CPU); both compute the same results
rem (the code builds without contracted multiply-adds). MK9_BUILD_MARCH: tests of the other one.
set "MARCH=x86-64-v2"
echo.| "%TOOLS%\clang.exe" -march=native -dM -E -x c - 2>nul | findstr /C:"__AVX2__" >nul && set "MARCH=x86-64-v3"
if defined MK9_BUILD_MARCH set "MARCH=%MK9_BUILD_MARCH%"

echo [1/3] Translating the game's code (codegen)...
pushd project || exit /b 1
..\sdk\bin\rexglue.exe codegen mk9_manifest.toml
if errorlevel 1 goto codegenfailed
popd

echo [2/3] Compiling (this takes a while the first time)...
>build\march.ninja echo march = %MARCH%
"%TOOLS%\ninja.exe" -f build.ninja
if errorlevel 1 goto compilefailed

echo [3/3] Installing...
copy /y host\mk9.exe mk9.exe >nul || goto cannotinstall
copy /y build\mk9game.dll mk9game.dll >nul || goto cannotinstall
copy /y sdk\bin\*.dll . >nul || goto cannotinstall
rem the release this build was made from: the launcher builds again after an update changes it
copy /y project\VERSION built.txt >nul
echo BUILD OK (%MARCH%)
exit /b 0

:nogame
echo The game's files are not there: extract your disc into the game folder here first.
exit /b 1

:missing
echo Part of MK9 Online is missing from this folder (host\mk9.exe, or clang or ninja in ..\..\toolchain\bin): an
echo antivirus may have taken it. Allow it there, or unpack the release again.
exit /b 1

:codegenfailed
popd
echo Translating the game's code failed.
exit /b 1

:compilefailed
echo Compiling the game failed.
exit /b 1

:cannotinstall
echo The game could not be put in place. Close it if it is still running, and build again. If it is not, an antivirus
echo may have taken mk9game.dll or mk9.exe: allow it there (Windows Security, Virus and threat protection, Protection
echo history) and build again.
exit /b 1
