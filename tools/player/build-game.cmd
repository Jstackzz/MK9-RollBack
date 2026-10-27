@echo off
rem Builds the native MK9 executable on this PC from the player's own disc (the Tekken 6 / MK3D way:
rem nobody ships the game's code, every PC translates its own copy). Run by the launcher's Build
rem game page once the disc is extracted into game\ beside this script, or by hand.
rem   1. rexglue codegen: game\default.xex -> project\generated (C++ for every function of the game)
rem   2. CMake + Ninja + clang: project -> build\mk9.exe, linked against the prebuilt runtime in sdk\
rem   3. mk9.exe and the runtime's DLLs next to this script
rem Needs Visual Studio Build Tools with "Desktop development with C++" (the MSVC libraries and the
rem Windows SDK; free from Microsoft). clang comes with the package in ..\..\toolchain.
setlocal EnableExtensions
set "HERE=%~dp0"
set "TOOLCHAIN=%HERE%..\..\toolchain"
if not exist "%HERE%game\default.xex" (
    echo The game's files are not there: extract your disc into %HERE%game first.
    exit /b 1
)

rem the newest Visual Studio (or Build Tools) with the C++ tools
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || goto novs
rem clang++ is clang under another name (the package keeps one copy)
if not exist "%TOOLCHAIN%\bin\clang++.exe" mklink /H "%TOOLCHAIN%\bin\clang++.exe" "%TOOLCHAIN%\bin\clang.exe" >nul 2>nul
if not exist "%TOOLCHAIN%\bin\clang++.exe" copy /y "%TOOLCHAIN%\bin\clang.exe" "%TOOLCHAIN%\bin\clang++.exe" >nul
set "PATH=%TOOLCHAIN%\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
where cmake >nul 2>nul || (echo CMake not found: add "C++ CMake tools for Windows" in the Visual Studio Installer. & exit /b 1)
where clang++ >nul 2>nul || (echo clang not found in %TOOLCHAIN%\bin. & exit /b 1)

rem AVX2 PCs get the faster code, older ones the SSE4 build (clang knows this PC's CPU); both
rem compute the same results (the project builds without contracted multiply-adds)
set "MARCH=x86-64-v2"
echo.| clang -march=native -dM -E -x c - 2>nul | findstr /C:"__AVX2__" >nul && set "MARCH=x86-64-v3"

echo [1/3] Translating the game's code (codegen)...
pushd "%HERE%project"
"%HERE%sdk\bin\rexglue.exe" codegen mk9_manifest.toml || (popd & echo codegen failed & exit /b 1)
popd

echo [2/3] Compiling (this takes a while the first time)...
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

:novs
echo Visual Studio Build Tools with C++ were not found. Install "Build Tools for Visual Studio" from
echo https://visualstudio.microsoft.com/downloads/ with the "Desktop development with C++" workload,
echo then build again.
exit /b 1
