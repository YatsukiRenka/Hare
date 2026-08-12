# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Yatsuki Renka

# Writes the cloud sync configuration to HKCU\Software\Rime\Hare\CloudSync.
#
# Credentials are stored with DPAPI, bound to the current Windows account, and
# deliberately kept out of the Rime user directory: that directory is itself
# synchronised, so a secret placed there would be uploaded and would also make
# reaching the cloud depend on the cloud.
#
# Nothing is written to disk by this script, and no credential appears in the
# repository. Pass them on the command line or leave the parameter out to be
# prompted.
#
#   pwsh tools\configure-sync.ps1 -Backend localdir -LocalDir D:\some\folder
#   pwsh tools\configure-sync.ps1 -Backend s3 -Endpoint https://<acct>.r2.cloudflarestorage.com -Bucket test
#   pwsh tools\configure-sync.ps1 -Backend webdav -DavUrl https://dav.jianguoyun.com/dav/test -DavUsername me@example.com
#   pwsh tools\configure-sync.ps1 -Backend worker -WorkerUrl https://hare-sync.<sub>.workers.dev
#
# Afterwards, establish the shared data key once per machine:
#
#   & "<install dir>\HareDeployer.exe" /cloudkey:<master password>

param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('none', 'localdir', 's3', 'webdav', 'worker')]
    [string]$Backend,

    [string]$LocalDir,

    [string]$Endpoint,
    [string]$Bucket,
    [string]$Prefix = 'hare/',
    [string]$AccessKeyId,
    [string]$SecretAccessKey,

    [string]$DavUrl,
    [string]$DavUsername,
    [string]$DavPassword,

    [string]$WorkerUrl,
    [string]$WorkerToken
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security -ErrorAction SilentlyContinue

$key = 'HKCU:\Software\Rime\Hare\CloudSync'
New-Item -Path $key -Force | Out-Null

function Set-Text([string]$name, [string]$value) {
    Set-ItemProperty -Path $key -Name $name -Value $value -Type String
}

function Set-Secret([string]$name, [string]$value) {
    if (-not $value) {
        $secure = Read-Host "$name" -AsSecureString
        $value = [Runtime.InteropServices.Marshal]::PtrToStringBSTR(
            [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure))
    }
    $protected = [Security.Cryptography.ProtectedData]::Protect(
        [Text.Encoding]::UTF8.GetBytes($value), $null,
        [Security.Cryptography.DataProtectionScope]::CurrentUser)
    Set-ItemProperty -Path $key -Name $name -Value $protected -Type Binary
}

Set-Text 'Backend' $Backend

switch ($Backend) {
    'localdir' {
        if (-not $LocalDir) { throw 'localdir needs -LocalDir' }
        Set-Text 'LocalDir' $LocalDir
    }
    's3' {
        if (-not $Endpoint -or -not $Bucket) { throw 's3 needs -Endpoint and -Bucket' }
        Set-Text 'Endpoint' $Endpoint
        Set-Text 'Bucket' $Bucket
        Set-Text 'Prefix' $Prefix
        Set-Secret 'AccessKeyId' $AccessKeyId
        Set-Secret 'SecretAccessKey' $SecretAccessKey
    }
    'webdav' {
        if (-not $DavUrl -or -not $DavUsername) { throw 'webdav needs -DavUrl and -DavUsername' }
        Set-Text 'DavUrl' $DavUrl
        Set-Text 'DavUsername' $DavUsername
        Set-Secret 'DavPassword' $DavPassword
    }
    'worker' {
        if (-not $WorkerUrl) { throw 'worker needs -WorkerUrl' }
        Set-Text 'WorkerUrl' $WorkerUrl
        Set-Secret 'WorkerToken' $WorkerToken
    }
}

# The cached data key belongs to whichever storage published it, so switching
# backends means rejoining the new one's key.
Remove-ItemProperty -Path $key -Name 'DataKey' -ErrorAction SilentlyContinue

Write-Host "backend set to $Backend"
Write-Host 'run HareDeployer.exe /cloudkey:<master password> to establish the data key'
