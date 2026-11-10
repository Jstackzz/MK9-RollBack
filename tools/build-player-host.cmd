@echo off
rem The player package's ready-made mk9.exe: the game's own code stays out of it, each player's PC compiles that part
rem from their own disc with nothing but the package (mk9\player\README.md). Built against dist\sdk, the runtime the
rem package ships, for any CPU the game runs on. Result in dist\host: mk9.exe, mk9.lib, player\ (for the player's link).
rem Usage: tools\build-player-host.cmd
call "%~dp0devenv.cmd"
set "OUT=%MK9_ROOT%\dist\host"
cd /d "%MK9_ROOT%\mk9"
if not exist "%OUT%\build.ninja" (
  cmake -S . -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
    "-DCMAKE_C_FLAGS=-march=x86-64-v2" "-DCMAKE_CXX_FLAGS=-march=x86-64-v2" ^
    "-DCMAKE_PREFIX_PATH=%MK9_ROOT%\dist\sdk" -DMK9_PLAYER_HOST=ON || exit /b 1
)
cmake --build "%OUT%" || exit /b 1
echo BUILD OK
