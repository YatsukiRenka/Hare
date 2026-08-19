# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Yatsuki Renka

# Writes the cloud sync configuration to HKCU\Software\Rime\Hare\CloudSync.
#
# The settings panel - HareDeployer.exe /settings, or the tray menu - does the
# same thing with a window. This script exists for setting up several machines
# without clicking through it.
#
# Credentials are stored with DPAPI, bound to the current Windows account, and
# deliberately kept out of the Rime user directory: that directory is itself
# synchronised, so a secret placed there would be uploaded and would also make
# reaching the cloud depend on the cloud.
#
# Nothing is written to disk by this script, and no credential appears in the
# repository. Credentials are accepted as SecureString values or prompted for
# securely when omitted.
#
#   pwsh tools\configure-sync.ps1 -Backend localdir -LocalDir D:\some\folder
#   pwsh tools\configure-sync.ps1 -Backend s3 -Endpoint https://<acct>.r2.cloudflarestorage.com -Bucket test
#   pwsh tools\configure-sync.ps1 -Backend webdav -DavUrl https://dav.jianguoyun.com/dav/test -DavUsername me@example.com
#   pwsh tools\configure-sync.ps1 -Backend worker -WorkerUrl https://hare-sync.<sub>.workers.dev
#
# Afterwards, establish the shared data key once per machine in the panel:
#
#   & "<install dir>\HareDeployer.exe" /settings
#
# The master password deliberately has no command line entry point: a command
# line is readable by every other process on the machine.

param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('none', 'localdir', 's3', 'webdav', 'worker')]
    [string]$Backend,

    [string]$LocalDir,

    [string]$Endpoint,
    [string]$Bucket,
    [string]$Prefix = 'hare/',
    [Security.SecureString]$AccessKeyId,
    [Security.SecureString]$SecretAccessKey,

    [string]$DavUrl,
    [string]$DavUsername,
    [Security.SecureString]$DavPassword,

    [string]$WorkerUrl,
    [Security.SecureString]$WorkerToken
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security -ErrorAction SilentlyContinue

$keyPath = 'Software\Rime\Hare\CloudSync'
$registryRoot = [Microsoft.Win32.Registry]::CurrentUser
$stringKind = [Microsoft.Win32.RegistryValueKind]::String
$binaryKind = [Microsoft.Win32.RegistryValueKind]::Binary

function Assert-Present([string]$name, [string]$value) {
    if ([string]::IsNullOrWhiteSpace($value)) {
        throw "$Backend needs -$name"
    }
}

function Protect-Secret([string]$name, [Security.SecureString]$secure) {
    $ownsSecureString = $false
    if ($null -eq $secure) {
        $secure = Read-Host $name -AsSecureString
        $ownsSecureString = $true
    }
    if ($secure.Length -eq 0) {
        if ($ownsSecureString) { $secure.Dispose() }
        throw "$name must not be empty"
    }

    $bstr = [IntPtr]::Zero
    $plain = $null
    $plainBytes = $null
    try {
        $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
        $plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
        $plainBytes = [Text.Encoding]::UTF8.GetBytes($plain)
        $protected = [Security.Cryptography.ProtectedData]::Protect(
            $plainBytes, $null,
            [Security.Cryptography.DataProtectionScope]::CurrentUser)
        if ($null -eq $protected -or $protected.Length -eq 0) {
            throw "failed to protect $name"
        }
        return ,$protected
    } finally {
        if ($null -ne $plainBytes) {
            [Array]::Clear($plainBytes, 0, $plainBytes.Length)
        }
        $plain = $null
        if ($bstr -ne [IntPtr]::Zero) {
            [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
        }
        if ($ownsSecureString) { $secure.Dispose() }
    }
}

function Normalize-S3Prefix([string]$value) {
    if ([string]::IsNullOrEmpty($value)) { return 'hare/' }
    if (-not $value.EndsWith('/')) { return "$value/" }
    $value
}

function Get-RegistryString($registryKey, [string]$name) {
    if ($null -eq $registryKey -or
        @($registryKey.GetValueNames()) -notcontains $name -or
        $registryKey.GetValueKind($name) -ne $stringKind) {
        return ''
    }
    [string]$registryKey.GetValue(
        $name, '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
}

# Keep these fields and their order in step with CloudSync.cpp's
# StorageIdentityRecord. Credentials are deliberately excluded; the WebDAV
# username is part of the remote namespace.
function Get-StoredStorageIdentityFields($registryKey) {
    $storedBackend = Get-RegistryString $registryKey 'Backend'
    switch ($storedBackend) {
        'localdir' {
            return @('localdir', (Get-RegistryString $registryKey 'LocalDir'))
        }
        's3' {
            $storedPrefix = Normalize-S3Prefix (
                Get-RegistryString $registryKey 'Prefix')
            return @('s3',
                (Get-RegistryString $registryKey 'Endpoint'),
                (Get-RegistryString $registryKey 'Bucket'), $storedPrefix)
        }
        'webdav' {
            return @('webdav',
                (Get-RegistryString $registryKey 'DavUrl'),
                (Get-RegistryString $registryKey 'DavUsername'))
        }
        'worker' {
            return @('worker', (Get-RegistryString $registryKey 'WorkerUrl'))
        }
        default { return @('none') }
    }
}

function Get-NextStorageIdentityFields {
    switch ($Backend) {
        'localdir' { return @('localdir', $LocalDir) }
        's3' { return @('s3', $Endpoint, $Bucket, $Prefix) }
        'webdav' { return @('webdav', $DavUrl, $DavUsername) }
        'worker' { return @('worker', $WorkerUrl) }
        default { return @('none') }
    }
}

function Get-StorageIdentityFingerprint([string[]]$fields) {
    $record = [IO.MemoryStream]::new()
    try {
        $preamble = [Text.Encoding]::UTF8.GetBytes("HARE-STORAGE-ID/1`0")
        $record.Write($preamble, 0, $preamble.Length)
        foreach ($field in $fields) {
            $fieldBytes = [Text.Encoding]::UTF8.GetBytes($field)
            $length = [uint32]$fieldBytes.Length
            $lengthBytes = [byte[]]@(
                [byte](($length -shr 24) -band 0xff),
                [byte](($length -shr 16) -band 0xff),
                [byte](($length -shr 8) -band 0xff),
                [byte]($length -band 0xff))
            $record.Write($lengthBytes, 0, $lengthBytes.Length)
            $record.Write($fieldBytes, 0, $fieldBytes.Length)
        }
        $recordBytes = $record.ToArray()
    } finally {
        $record.Dispose()
    }

    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        return ,$sha256.ComputeHash($recordBytes)
    } finally {
        $sha256.Dispose()
    }
}

function Get-RegistrySnapshot($registryKey, [string[]]$names) {
    $snapshot = @{}
    $existingNames = if ($null -eq $registryKey) {
        @()
    } else {
        @($registryKey.GetValueNames())
    }
    foreach ($name in @($names | Select-Object -Unique)) {
        if ($existingNames -contains $name) {
            $value = $registryKey.GetValue(
                $name, $null,
                [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            if ($value -is [Array]) { $value = $value.Clone() }
            $snapshot[$name] = [pscustomobject]@{
                Exists = $true
                Value = $value
                Kind = $registryKey.GetValueKind($name)
            }
        } else {
            $snapshot[$name] = [pscustomobject]@{ Exists = $false }
        }
    }
    $snapshot
}

function Restore-RegistrySnapshot([hashtable]$snapshot, [bool]$keyExisted) {
    if (-not $keyExisted) {
        $registryRoot.DeleteSubKeyTree($keyPath, $false)
        return
    }

    $restoreKey = $registryRoot.CreateSubKey($keyPath)
    if ($null -eq $restoreKey) { throw 'failed to reopen configuration key for rollback' }
    try {
        foreach ($name in $snapshot.Keys) {
            $saved = $snapshot[$name]
            if ($saved.Exists) {
                $restoreKey.SetValue($name, $saved.Value, $saved.Kind)
            } else {
                $restoreKey.DeleteValue($name, $false)
            }
        }
        $restoreKey.Flush()
    } finally {
        $restoreKey.Dispose()
    }
}

# Validate ordinary fields before prompting for or protecting any credential.
switch ($Backend) {
    'localdir' { Assert-Present 'LocalDir' $LocalDir }
    's3' {
        Assert-Present 'Endpoint' $Endpoint
        Assert-Present 'Bucket' $Bucket
        $Prefix = Normalize-S3Prefix $Prefix
    }
    'webdav' {
        Assert-Present 'DavUrl' $DavUrl
        Assert-Present 'DavUsername' $DavUsername
    }
    'worker' { Assert-Present 'WorkerUrl' $WorkerUrl }
}

# Stage every value, including DPAPI output, before the registry is touched.
$staged = [ordered]@{}
switch ($Backend) {
    'localdir' {
        $staged['LocalDir'] = [pscustomobject]@{ Value = $LocalDir; Kind = $stringKind }
    }
    's3' {
        $staged['Endpoint'] = [pscustomobject]@{ Value = $Endpoint; Kind = $stringKind }
        $staged['Bucket'] = [pscustomobject]@{ Value = $Bucket; Kind = $stringKind }
        $staged['Prefix'] = [pscustomobject]@{ Value = $Prefix; Kind = $stringKind }
        $staged['AccessKeyId'] = [pscustomobject]@{
            Value = Protect-Secret 'AccessKeyId' $AccessKeyId
            Kind = $binaryKind
        }
        $staged['SecretAccessKey'] = [pscustomobject]@{
            Value = Protect-Secret 'SecretAccessKey' $SecretAccessKey
            Kind = $binaryKind
        }
    }
    'webdav' {
        $staged['DavUrl'] = [pscustomobject]@{ Value = $DavUrl; Kind = $stringKind }
        $staged['DavUsername'] = [pscustomobject]@{ Value = $DavUsername; Kind = $stringKind }
        $staged['DavPassword'] = [pscustomobject]@{
            Value = Protect-Secret 'DavPassword' $DavPassword
            Kind = $binaryKind
        }
    }
    'worker' {
        $staged['WorkerUrl'] = [pscustomobject]@{ Value = $WorkerUrl; Kind = $stringKind }
        $staged['WorkerToken'] = [pscustomobject]@{
            Value = Protect-Secret 'WorkerToken' $WorkerToken
            Kind = $binaryKind
        }
    }
}
$staged['Backend'] = [pscustomobject]@{ Value = $Backend; Kind = $stringKind }

$existingKey = $null
try {
    $existingKey = $registryRoot.OpenSubKey($keyPath, $false)
    $keyExisted = $null -ne $existingKey
    $previousIdentity = Get-StorageIdentityFingerprint (
        Get-StoredStorageIdentityFields $existingKey)
    $nextIdentity = Get-StorageIdentityFingerprint (Get-NextStorageIdentityFields)
    $identityChanged = -not [Linq.Enumerable]::SequenceEqual(
        [byte[]]$previousIdentity, [byte[]]$nextIdentity)
    $staged['DataKeyIdentity'] = [pscustomobject]@{
        Value = $nextIdentity
        Kind = $binaryKind
    }
    $affectedNames = @($staged.Keys)
    if ($identityChanged) { $affectedNames += 'DataKey' }
    $snapshot = Get-RegistrySnapshot $existingKey $affectedNames
} finally {
    if ($null -ne $existingKey) { $existingKey.Dispose() }
}

$targetKey = $null
$commitStarted = $false
try {
    $targetKey = $registryRoot.CreateSubKey($keyPath)
    if ($null -eq $targetKey) { throw 'failed to open configuration key' }
    $commitStarted = $true
    if ($identityChanged) {
        foreach ($name in @('DataKeyIdentity', 'DataKey')) {
            $targetKey.DeleteValue($name, $false)
            if (@($targetKey.GetValueNames()) -contains $name) {
                throw "failed to invalidate cached data key value $name"
            }
        }
    }
    foreach ($entry in $staged.GetEnumerator()) {
        $targetKey.SetValue($entry.Key, $entry.Value.Value, $entry.Value.Kind)
    }
    $targetKey.Flush()
} catch {
    $commitError = $_
    if ($null -ne $targetKey) {
        $targetKey.Dispose()
        $targetKey = $null
    }
    if ($commitStarted) {
        try {
            Restore-RegistrySnapshot $snapshot $keyExisted
        } catch {
            throw "configuration commit failed and rollback failed: $($_.Exception.Message)"
        }
    }
    throw $commitError
} finally {
    if ($null -ne $targetKey) { $targetKey.Dispose() }
}

Write-Host "backend set to $Backend"
Write-Host 'run HareDeployer.exe /settings to establish the data key'
