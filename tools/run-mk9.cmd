@echo off
rem Start the dev build of the game. Usage: tools\run-mk9.cmd [log name] [extra --cvar=value ...]
rem The log goes to mk9\out\build\win-amd64-release\logs\<log name>.log (default run).
setlocal
set "ROOT=%~dp0.."
set "BIN=%ROOT%\mk9\out\build\win-amd64-release"
set "NAME=%~1"
if "%NAME%"=="" set "NAME=run"
shift
set "EXTRA="
:args
if "%~1"=="" goto go
set "EXTRA=%EXTRA% %1"
shift
goto args
:go
if not exist "%BIN%\logs" mkdir "%BIN%\logs"
start "" /D "%BIN%" "%BIN%\mk9.exe" "--game_data_root=%ROOT%\game\disc" "--user_data_root=%ROOT%\game\user" "--log_file=%BIN%\logs\%NAME%.log" %EXTRA%
endlocal
