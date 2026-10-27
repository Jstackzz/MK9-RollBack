@echo off
rem Build the MK9 Online launcher (Release) into build\launcher and copy the Qt runtime next to it.
rem Qt comes from external\qt\6.12.0\msvc2022_64 unless QT_DIR points elsewhere; SDL3 from the ReXGlue SDK's copy
rem (external\rexglue-sdk\thirdparty\sdl3) unless SDL3_DIR does; BUILD_DIR picks another build folder. Nothing is
rem downloaded. See docs\LAUNCHER.md.
setlocal
set "ROOT=%~dp0.."
if "%QT_DIR%"=="" set "QT_DIR=%ROOT%\external\qt\6.12.0\msvc2022_64"
if "%SDL3_DIR%"=="" set "SDL3_DIR=%ROOT%\external\rexglue-sdk\thirdparty\sdl3"
if "%BUILD_DIR%"=="" set "BUILD_DIR=%ROOT%\build\launcher"
if not exist "%QT_DIR%\bin\windeployqt.exe" (
    echo Qt not found in %QT_DIR%. Install it first, see docs\LAUNCHER.md.
    exit /b 1
)
if not exist "%SDL3_DIR%\CMakeLists.txt" (
    echo SDL3 sources not found in %SDL3_DIR%. Set SDL3_DIR, see docs\LAUNCHER.md.
    exit /b 1
)
call "%ROOT%\tools\vsenv.cmd" cmake -S "%ROOT%\launcher" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=%QT_DIR%" "-DMK9_SDL3_DIR=%SDL3_DIR%"
if errorlevel 1 exit /b 1
call "%ROOT%\tools\vsenv.cmd" cmake --build "%BUILD_DIR%"
if errorlevel 1 exit /b 1
"%QT_DIR%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "%BUILD_DIR%\MK9Launcher.exe"
if errorlevel 1 exit /b 1
echo.
echo Built %BUILD_DIR%\MK9Launcher.exe
