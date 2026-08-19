@echo off
rem SPDX-License-Identifier: AGPL-3.0-or-later
rem Copyright (C) 2026 Yatsuki Renka
rem Build and run the cloud sync core tests, then compile the production
rem deployer, without invoking build.bat.

setlocal

set "NoDefaultCurrentDirectoryInExePath="
rem MSBuild treats environment names case-insensitively and rejects duplicates.
set "http_proxy="
set "https_proxy="
rem VS 2026 18.9 file tracking can leave CL.exe suspended after Tracker exits.
rem This validation gate does not depend on incremental file-access tracking.
set "TrackFileAccess=false"

pushd "%~dp0.."
set "HARE_ROOT=%CD%"
call env.bat
if errorlevel 1 goto error

if defined VSPATH if exist "%VSPATH%\VC\Tools\MSVC\*\atlmfc\include\atlbase.h" goto have_vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: vswhere.exe not found, cannot locate Visual Studio
  goto error
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -all -prerelease -products * -sort -property installationPath`) do (
  if not defined VSPATH (
    for /d %%t in ("%%i\VC\Tools\MSVC\*") do (
      if exist "%%~t\atlmfc\include\atlbase.h" set "VSPATH=%%i"
    )
  )
)
if not defined VSPATH (
  echo ERROR: no Visual Studio installation with the C++ ATL headers was found
  goto error
)

:have_vs
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 goto error

if not defined VERSION_MAJOR set "VERSION_MAJOR=0"
if not defined VERSION_MINOR set "VERSION_MINOR=17"
if not defined VERSION_PATCH set "VERSION_PATCH=4"
if not defined PRODUCT_VERSION set "PRODUCT_VERSION=0.17.4.0"
if not defined FILE_VERSION set "FILE_VERSION=0.17.4.0"

set WEASEL_PROJECT_PROPERTIES=BOOST_ROOT^
  PLATFORM_TOOLSET^
  VERSION_MAJOR^
  VERSION_MINOR^
  VERSION_PATCH^
  PRODUCT_VERSION^
  FILE_VERSION
cscript.exe render.js weasel.props %WEASEL_PROJECT_PROPERTIES%
if errorlevel 1 goto error

for %%p in (x64 Win32) do (
  msbuild.exe test\TestCloudSyncCore\TestCloudSyncCore.vcxproj /t:Build /p:Configuration=Debug /p:Platform=%%p /nr:false /nologo /verbosity:minimal
  if errorlevel 1 goto error
  "msbuild\Debug\%%p\TestCloudSyncCore.exe"
  if errorlevel 1 goto error
  msbuild.exe weasel.sln /t:WeaselDeployer /p:Configuration=Release /p:Platform=%%p /nr:false /nologo /verbosity:minimal
  if errorlevel 1 goto error
)

if /i "%~1" == "full" (
  for %%p in (x64 Win32) do (
    msbuild.exe weasel.sln /t:Build /p:Configuration=Release /p:Platform=%%p /nr:false /nologo /verbosity:minimal
    if errorlevel 1 goto error
  )
)

popd
exit /b 0

:error
popd
exit /b 1
