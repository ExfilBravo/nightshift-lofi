@echo off
rem Builds Nightshift Lofi with the MSVC toolchain.
rem Usage: build.cmd [Release^|Debug] [target] [build-subfolder]
rem   target          a CMake target (NightshiftCassette_VST3, lofitool, ...); default: everything
rem   build-subfolder a separate build tree, for building one plugin beside another build
setlocal
set CFG=%~1
if "%CFG%"=="" set CFG=Release
set TARGET=%~2
set SUB=%~3
if "%SUB%"=="" set SUB=%CFG%

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" (echo Visual Studio C++ build tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cmake -S "%~dp0." -B "%~dp0build\%SUB%" -G Ninja -DCMAKE_BUILD_TYPE=%CFG% || exit /b 1
if "%TARGET%"=="" (
    cmake --build "%~dp0build\%SUB%" || exit /b 1
) else (
    cmake --build "%~dp0build\%SUB%" --target %TARGET% || exit /b 1
)
echo Built in %~dp0build\%SUB%
