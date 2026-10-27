@echo off
rem Configure and build the ReXGlue SDK's rexglue CLI (Release). Usage: tools\build-sdk.cmd [target]
call "%~dp0devenv.cmd"
set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=rexglue"
cd /d "%MK9_ROOT%\external\rexglue-sdk"
if not exist out\build\win-amd64\build.ninja (
  cmake --preset win-amd64 || exit /b 1
)
cmake --build out\build\win-amd64 --config Release --target %TARGET% || exit /b 1
echo BUILD OK
