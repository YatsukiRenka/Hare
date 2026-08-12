@echo off
rem SPDX-License-Identifier: AGPL-3.0-or-later
rem Copyright (C) 2026 Yatsuki Renka
rem Build the Boost static libraries that Hare links against.
rem
rem Only serialization and thread are needed. librime arrives prebuilt through
rem get-rime.ps1 and carries its own dependencies, so the rest of Boost is left
rem alone; this is why the upstream "--build-type=complete" sweep is not used.
rem
rem Expects the Boost source tree at deps\boost_1_91_0, which install-deps.ps1
rem downloads and extracts.

setlocal

rem Some shells set NoDefaultCurrentDirectoryInExePath, which stops cmd from
rem resolving batch files in the working directory. Boost's bootstrap invokes
rem guess_toolset.bat that way, so clear it for the duration of the build.
set "NoDefaultCurrentDirectoryInExePath="

pushd "%~dp0.."
set "HARE_ROOT=%CD%"
popd

set "BOOST_DIR=%HARE_ROOT%\deps\boost_1_91_0"
if not exist "%BOOST_DIR%\bootstrap.bat" (
  echo ERROR: Boost source not found at %BOOST_DIR%
  echo Run tools\install-deps.ps1 first.
  exit /b 1
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
rem Pick the same installation build-hare.bat uses: newest one carrying ATL.
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -all -prerelease -products * -sort -property installationPath`) do (
  if not defined VSPATH (
    for /d %%t in ("%%i\VC\Tools\MSVC\*") do (
      if exist "%%~t\atlmfc\include\atlbase.h" set "VSPATH=%%i"
    )
  )
)
if not defined VSPATH (
  echo ERROR: no Visual Studio installation with the C++ toolset was found
  exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1

pushd "%BOOST_DIR%"

if not exist "%BOOST_DIR%\b2.exe" (
  echo === bootstrapping b2 ===
  rem MSVC 14.51 from Visual Studio 2026 is vc145 in Boost's toolset naming.
  call "%BOOST_DIR%\bootstrap.bat" vc145
)

if not exist "%BOOST_DIR%\b2.exe" (
  echo ERROR: b2.exe was not produced by bootstrap
  popd
  exit /b 1
)

set BOOST_OPTS=-j%NUMBER_OF_PROCESSORS% --with-serialization --with-thread ^
 toolset=msvc link=static runtime-link=static variant=release ^
 define=BOOST_USE_WINAPI_VERSION=0x0603 architecture=x86

echo.
echo === building x64 ===
"%BOOST_DIR%\b2.exe" %BOOST_OPTS% address-model=64 stage
if errorlevel 1 goto fail

echo.
echo === building Win32 ===
"%BOOST_DIR%\b2.exe" %BOOST_OPTS% address-model=32 stage
if errorlevel 1 goto fail

popd
echo.
echo BOOST BUILD OK
exit /b 0

:fail
popd
echo BOOST BUILD FAILED
exit /b 1
