@echo off
rem SPDX-License-Identifier: AGPL-3.0-or-later
rem Copyright (C) 2026 Yatsuki Renka
rem Wrapper around the upstream build.bat.
rem
rem Arguments are forwarded to build.bat (weasel, installer, rebuild, debug, ...).
rem Running it with no arguments builds the weasel projects for x64 and Win32.
rem
rem Two environment details it takes care of:
rem   - Some shells set NoDefaultCurrentDirectoryInExePath, which stops cmd from
rem     resolving batch files in the working directory. build.bat relies on
rem     "call env.bat", so the variable is cleared here.
rem   - build.bat expects to run inside a Visual Studio developer environment.

setlocal

set "NoDefaultCurrentDirectoryInExePath="

pushd "%~dp0.."
set "HARE_ROOT=%CD%"
popd

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: vswhere.exe not found, cannot locate Visual Studio
  exit /b 1
)

rem Pick the newest installation that actually carries ATL. Querying for the
rem component id is unreliable, since VS2026 does not report VC.ATL, and
rem -latest on its own can land on a Build Tools instance with no ATL headers.
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -all -prerelease -products * -sort -property installationPath`) do (
  if not defined VSPATH (
    for /d %%t in ("%%i\VC\Tools\MSVC\*") do (
      if exist "%%~t\atlmfc\include\atlbase.h" set "VSPATH=%%i"
    )
  )
)
if not defined VSPATH (
  echo ERROR: no Visual Studio installation with the C++ ATL headers was found
  echo Install the "C++ ATL for latest build tools" component.
  exit /b 1
)
echo using Visual Studio at %VSPATH%

call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1

pushd "%HARE_ROOT%"
call "%HARE_ROOT%\build.bat" %*
set BUILD_RESULT=%ERRORLEVEL%
popd

exit /b %BUILD_RESULT%
