@echo off
setlocal EnableExtensions

cd /d "%~dp0"

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=debug"

if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
if exist "%ProgramFiles%\LLVM\bin\clang++.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload.
    exit /b 1
)

set "VCVARS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set "VCVARS=%%i"

if not defined VCVARS (
    echo vcvars64.bat not found. Install Visual Studio Build Tools with the C++ workload.
    exit /b 1
)

call "%VCVARS%"
if errorlevel 1 exit /b 1

cmake --preset "%PRESET%"
if errorlevel 1 exit /b 1

cmake --build --preset "%PRESET%"
if errorlevel 1 exit /b 1

ctest --preset "%PRESET%"
if errorlevel 1 exit /b 1
