# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Yatsuki Renka

# Fetches the third-party pieces the build needs and seeds the shared data
# directory. Safe to re-run; anything already present is skipped.
#
#   1. prebuilt librime, via the upstream get-rime.ps1
#   2. Boost source into deps\boost_1_91_0
#   3. the WebView2 SDK into deps\webview2, for the settings panel
#   4. output\data, copied from an installed Weasel if one is available
#
# Boost still has to be compiled afterwards with tools\build-boost.bat.

param(
    [string]$BoostVersion = '1.91.0',
    [string]$WebView2Version = '1.0.4129.50',
    # Any directory holding a Weasel/Rime shared data folder, used to seed
    # output\data so build.bat does not have to run plum over the network.
    [string]$SharedDataSource = 'D:\App\Rime\weasel-0.17.4\data'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

Push-Location $root
try {
    # --- 7z, needed to unpack both librime and Boost -----------------------
    $sevenZip = (Get-Command 7z -ErrorAction SilentlyContinue).Source
    if (-not $sevenZip) {
        $candidates = @(
            'D:\App\Rime\weasel-0.17.4\7z.exe',
            "$env:ProgramFiles\7-Zip\7z.exe"
        )
        $sevenZip = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    }
    if (-not $sevenZip) {
        throw '7z.exe not found. Install 7-Zip or put 7z.exe on PATH.'
    }
    $env:Path = "$(Split-Path -Parent $sevenZip);$env:Path"

    # --- prebuilt librime ---------------------------------------------------
    foreach ($d in 'include', 'lib', 'lib64', 'output\Win32', 'output\data\opencc') {
        New-Item -ItemType Directory -Force -Path (Join-Path $root $d) | Out-Null
    }
    if (Test-Path (Join-Path $root 'lib64\rime.lib')) {
        Write-Host 'librime already present, skipping'
    } else {
        Write-Host 'fetching prebuilt librime'
        & (Join-Path $root 'get-rime.ps1') -use dev
    }

    # --- Boost source -------------------------------------------------------
    $boostUnderscored = $BoostVersion.Replace('.', '_')
    $boostDir = Join-Path $root "deps\boost_$boostUnderscored"
    if (Test-Path (Join-Path $boostDir 'boost')) {
        Write-Host "Boost $BoostVersion already extracted, skipping"
    } else {
        New-Item -ItemType Directory -Force -Path (Join-Path $root 'deps') | Out-Null
        $archive = Join-Path $root "deps\boost_$boostUnderscored.7z"
        if (-not (Test-Path $archive)) {
            $url = "https://archives.boost.io/release/$BoostVersion/source/boost_$boostUnderscored.7z"
            Write-Host "downloading $url"
            curl.exe -sSL --retry 2 --fail -o $archive $url
            if ($LASTEXITCODE -ne 0) { throw "failed to download Boost $BoostVersion" }
        }
        Write-Host 'extracting Boost'
        & $sevenZip x $archive "-o$(Join-Path $root 'deps')" -y | Out-Null
    }

    # --- WebView2 SDK -------------------------------------------------------
    # Only the headers and the static loader are taken. Linking the loader
    # statically means nothing extra has to be installed next to the executable;
    # the runtime itself ships with Windows 11 and is a separate download on
    # Windows 10, which the panel reports rather than bundling.
    $webview2Dir = Join-Path $root 'deps\webview2'
    if (Test-Path (Join-Path $webview2Dir 'include\WebView2.h')) {
        Write-Host 'WebView2 SDK already present, skipping'
    } else {
        $package = Join-Path $root "deps\microsoft.web.webview2.$WebView2Version.nupkg"
        if (-not (Test-Path $package)) {
            $url = "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$WebView2Version"
            Write-Host "downloading $url"
            curl.exe -sSL --retry 2 --fail -o $package $url
            if ($LASTEXITCODE -ne 0) { throw "failed to download WebView2 SDK $WebView2Version" }
        }
        # x86 lands in Win32 because that is the platform name MSBuild uses, so
        # the project can point at deps\webview2\$(Platform) unconditionally.
        $wanted = @{
            'build/native/include/WebView2.h'                   = 'include\WebView2.h'
            'build/native/include/WebView2EnvironmentOptions.h' = 'include\WebView2EnvironmentOptions.h'
            'build/native/x64/WebView2LoaderStatic.lib'         = 'x64\WebView2LoaderStatic.lib'
            'build/native/x86/WebView2LoaderStatic.lib'         = 'Win32\WebView2LoaderStatic.lib'
        }
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip = [IO.Compression.ZipFile]::OpenRead($package)
        try {
            foreach ($pair in $wanted.GetEnumerator()) {
                $entry = $zip.GetEntry($pair.Key)
                if (-not $entry) { throw "$($pair.Key) missing from the WebView2 package" }
                $target = Join-Path $webview2Dir $pair.Value
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
            }
        } finally {
            $zip.Dispose()
        }
        Write-Host "WebView2 SDK $WebView2Version extracted"
    }

    # --- shared data --------------------------------------------------------
    $dataDir = Join-Path $root 'output\data'
    if (Test-Path (Join-Path $dataDir 'essay.txt')) {
        Write-Host 'output\data already seeded, skipping'
    } elseif (Test-Path $SharedDataSource) {
        Write-Host "seeding output\data from $SharedDataSource"
        Copy-Item "$SharedDataSource\*" $dataDir -Recurse -Force
    } else {
        Write-Warning "no shared data at $SharedDataSource; build.bat will run plum to fetch it"
    }

    Write-Host ''
    Write-Host 'dependencies ready. Next: tools\build-boost.bat, then tools\build-hare.bat'
} finally {
    Pop-Location
}
