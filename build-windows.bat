@echo off
rem Build squish.dll + squish.exe with MSVC (native Windows build).
rem Equivalent to 'make windows-dll'; see also 'make dll' for mingw-w64.
rem
rem Works from a plain command prompt: if cl.exe is not already on PATH
rem (e.g. a VS Developer Command Prompt), locates Visual Studio via
rem vswhere and initializes the amd64 build environment.

setlocal
cd /d "%~dp0"

where cl.exe >nul 2>nul
if %errorlevel%==0 goto :build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo error: cl.exe not on PATH and vswhere.exe not found.
    echo Install Visual Studio with the C++ workload, or run this from a
    echo "Developer Command Prompt for VS".
    exit /b 1
)

for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo error: no Visual Studio installation with C++ tools found.
    exit /b 1
)

call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -no_logo
if errorlevel 1 (
    echo error: failed to initialize the Visual Studio environment.
    exit /b 1
)

:build
rem squish.exe links the library statically (squish.c compiled straight in) so
rem it stands alone: 'squish s' self-extracting archives copy the CLI as their
rem stub and must run without squish.dll present.
cl /nologo /O2 /W3 /LD /DSQUISH_BUILD_DLL /Fe:squish.dll squish.c
if errorlevel 1 exit /b 1
cl /nologo /O2 /W3 /Fe:squish.exe squish_cli.c squish.c
if errorlevel 1 exit /b 1

echo.
echo Built squish.dll and squish.exe
