@echo off
rem Build the native MK9 executable. Usage: mk9\build.cmd [release|relwithdebinfo|debug]
rem The SDK is built from external\rexglue-sdk as part of this project (REXSDK_DIR).
call "%~dp0..\tools\devenv.cmd"
set "CFG=%~1"
if "%CFG%"=="" set "CFG=release"
cd /d "%~dp0"
if not exist "out\build\win-amd64-%CFG%\build.ninja" (
  cmake --preset win-amd64-%CFG% "-DREXSDK_DIR=%MK9_ROOT%\external\rexglue-sdk" || exit /b 1
)
cmake --build --preset win-amd64-%CFG% || exit /b 1
echo BUILD OK
