# Fetches the third-party pieces the build needs and seeds the shared data
# directory. Safe to re-run; anything already present is skipped.
#
#   1. prebuilt librime, via the upstream get-rime.ps1
#   2. Boost source into deps\boost_1_91_0
#   3. output\data, copied from an installed Weasel if one is available
#
# Boost still has to be compiled afterwards with tools\build-boost.bat.

param(
    [string]$BoostVersion = '1.91.0',
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
