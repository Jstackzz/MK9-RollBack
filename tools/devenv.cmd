@echo off
rem Developer environment: MSVC libraries and Windows SDK from VS Build Tools, clang from
rem external\llvm, CMake and Ninja from VS. Usage: call tools\devenv.cmd
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "MK9_ROOT=%~dp0.."
set "PATH=%MK9_ROOT%\external\llvm\bin;C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
