@echo off
rem Builds the native MK9 executable on this PC from the player's own disc (the Tekken 6 / MK3D way:
rem nobody ships the game's code, every PC translates its own copy). Run by the launcher's Build
rem game page once the disc is extracted into game\ beside this script, or by hand.
rem   1. rexglue codegen: game\default.xex -> project\generated (C++ for every function of the game)
rem   2. CMake + Ninja + clang: project -> build\mk9.exe, linked against the prebuilt runtime in sdk\
rem   3. mk9.exe and the runtime's DLLs next to this script
rem Needs Visual Studio Build Tools with "Desktop development with C++" (the MSVC libraries and the
rem Windows SDK; free from Microsoft). clang comes with the package in ..\..\toolchain.
rem This script never expands a path inside a ( ) block, where a ")" in it would end the block, and
rem quotes the paths it prints; the compile itself cannot work in a folder whose path has ( ) & ^ or %%
rem (a second download's "MK9Online-0.3.1 (1)"), and says so.
setlocal EnableExtensions
set "HERE=%~dp0"
set "TOOLCHAIN=%HERE%..\..\toolchain"
if not exist "%HERE%game\default.xex" goto nogame

rem cmd.exe takes ( ) & ^ %% for its own, and the link step runs through it with the compiler's short path
rem unquoted ("MK9ONL~1.1(1" for "MK9Online-0.3.1 (1)"): such a folder cannot build (a junction to a plain
rem name works in some places and not in others, so the player renames the folder instead)
echo "%HERE%"| findstr /l /c:"(" /c:")" /c:"&" /c:"^" /c:"%%" >nul && goto badpath

rem the newest Visual Studio (or Build Tools) with the C++ tools
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
rem vcvars runs vswhere by name too: on the PATH, or it prints "'vswhere.exe' is not recognized", which reads
rem like the failure when a build stops later
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || goto novs
rem clang++ is clang under another name (the package keeps one copy)
if not exist "%TOOLCHAIN%\bin\clang++.exe" mklink /H "%TOOLCHAIN%\bin\clang++.exe" "%TOOLCHAIN%\bin\clang.exe" >nul 2>nul
if not exist "%TOOLCHAIN%\bin\clang++.exe" copy /y "%TOOLCHAIN%\bin\clang.exe" "%TOOLCHAIN%\bin\clang++.exe" >nul
set "PATH=%TOOLCHAIN%\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
where cmake >nul 2>nul || goto nocmake
where clang++ >nul 2>nul || goto noclang

rem AVX2 PCs get the faster code, older ones the SSE4 build (clang knows this PC's CPU); both
rem compute the same results (the project builds without contracted multiply-adds)
set "MARCH=x86-64-v2"
echo.| clang -march=native -dM -E -x c - 2>nul | findstr /C:"__AVX2__" >nul && set "MARCH=x86-64-v3"

echo [1/3] Translating the game's code (codegen)...
pushd "%HERE%project"
"%HERE%sdk\bin\rexglue.exe" codegen mk9_manifest.toml || (popd & echo codegen failed & exit /b 1)
popd

echo [2/3] Compiling (this takes a while the first time)...
rem a build folder configured at another path (the package moved or renamed) is one CMake refuses: it
rem starts over
set "BUILDDIR=%HERE%build"
set "BUILDDIR=%BUILDDIR:\=/%"
if exist "%HERE%build\CMakeCache.txt" findstr /b /i /l /c:"CMAKE_CACHEFILE_DIR:INTERNAL=%BUILDDIR%" "%HERE%build\CMakeCache.txt" >nul || rmdir /s /q "%HERE%build"
cmake -S "%HERE%project" -B "%HERE%build" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
    "-DCMAKE_C_FLAGS=-march=%MARCH%" "-DCMAKE_CXX_FLAGS=-march=%MARCH%" ^
    "-DCMAKE_PREFIX_PATH=%HERE%sdk" || (echo configure failed & exit /b 1)
cmake --build "%HERE%build" || (echo compile failed & exit /b 1)

echo [3/3] Installing...
copy /y "%HERE%build\mk9.exe" "%HERE%mk9.exe" >nul || exit /b 1
for %%f in ("%HERE%sdk\bin\*.dll") do copy /y "%%f" "%HERE%" >nul
rem the release this build was made from: the launcher builds again after an update changes it
if exist "%HERE%project\VERSION" copy /y "%HERE%project\VERSION" "%HERE%built.txt" >nul
echo BUILD OK (%MARCH%)
exit /b 0

:badpath
echo The compiler's tools cannot work in this folder: its path has ( ) ^& ^^ or %% in it: "%HERE%"
echo Close the launcher, rename the folder with it (for example "MK9Online-0.3.1 (1)" to "MK9Online"),
echo then open the launcher again and build.
exit /b 1

:nogame
echo The game's files are not there: extract your disc into "%HERE%game" first.
exit /b 1

:nocmake
echo CMake not found: add "C++ CMake tools for Windows" in the Visual Studio Installer.
exit /b 1

:noclang
echo clang not found in "%TOOLCHAIN%\bin".
exit /b 1

:novs
echo Visual Studio Build Tools with C++ were not found. Install "Build Tools for Visual Studio" from
echo https://visualstudio.microsoft.com/downloads/ with the "Desktop development with C++" workload,
echo then build again.
exit /b 1
