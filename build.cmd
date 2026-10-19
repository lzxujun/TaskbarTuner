@echo off
rem ---------------------------------------------------------------------------
rem TaskbarTuner - build script
rem
rem Needs: Visual Studio 2019/2022 with the "Desktop development with C++"
rem workload. CMake is used when it is installed; otherwise the compiler is
rem driven directly by tools\build_cl.py, which needs nothing but Python.
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

echo.
echo === Taskbar Tuner build ===
echo.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)

if not defined VSPATH (
    echo [ERROR] No Visual Studio C++ toolchain found.
    echo.
    echo Install "Visual Studio Build Tools" and tick these components:
    echo   - Desktop development with C++
    echo   - Windows 10/11 SDK
    echo.
    echo https://visualstudio.microsoft.com/downloads/  ^(Build Tools for Visual Studio^)
    echo.
    exit /b 1
)

echo Toolchain : %VSPATH%

rem The MSVC environment provides the compiler, the resource compiler and the
rem SDK include/lib paths.
call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 -no_logo >nul 2>&1

set "OUT=build\bin\TaskbarTuner.exe"
set "CMAKE="
where /q cmake && set "CMAKE=cmake"
if not defined CMAKE if exist "%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if defined CMAKE goto :cmake

rem --- no CMake: drive cl.exe directly -------------------------------------
set "PY="
where /q python && set "PY=python"
if not defined PY where /q py && set "PY=py -3"
if not defined PY (
    echo [ERROR] Neither CMake nor Python is available.
    echo Install the "C++ CMake tools for Windows" component, or Python.
    exit /b 1
)

rem VCToolsInstallDir and WindowsSdkDir end with a backslash, which would
rem escape the closing quote. The "." + %%~fi form removes it.
set "VCDIR="
if defined VCToolsInstallDir for %%i in ("%VCToolsInstallDir%.") do set "VCDIR=%%~fi"
set "SDKDIR="
if defined WindowsSdkDir for %%i in ("%WindowsSdkDir%.") do set "SDKDIR=%%~fi"

if not defined VCDIR (
    echo [ERROR] VCToolsInstallDir is not set; run this from a VS developer prompt.
    exit /b 1
)
if not defined SDKDIR (
    echo [ERROR] WindowsSdkDir is not set; the Windows SDK is missing.
    exit /b 1
)

echo CMake      : not installed, using tools\build_cl.py
%PY% tools\build_cl.py --vc "%VCDIR%" --sdk "%SDKDIR%"
if errorlevel 1 goto :fail
goto :done

:cmake
echo CMake      : %CMAKE%
echo.
"%CMAKE%" -S . -B build -A x64
if errorlevel 1 goto :fail
"%CMAKE%" --build build --config Release
if errorlevel 1 goto :fail

:done
if not exist "%OUT%" (
    echo [ERROR] Build finished but %OUT% is missing.
    exit /b 1
)

echo.
echo === Done ===
for %%f in ("%OUT%") do echo %%~ff  ^(%%~zf bytes^)
echo.
echo Run it, then use the tray icon. The log is at
echo   %%LOCALAPPDATA%%\TaskbarTuner\TaskbarTuner.log
exit /b 0

:fail
echo.
echo [ERROR] Build failed. The messages above say which target and which file.
exit /b 1
