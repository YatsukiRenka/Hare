# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Yatsuki Renka

# Minimal S3 client for inspecting a sync bucket during development.
# Credentials come from parameters or the environment; nothing is stored here.
#
#   $env:HARE_S3_ENDPOINT / HARE_S3_BUCKET / HARE_S3_KEY / HARE_S3_SECRET
#
#   pwsh tools\s3.ps1 list
#   pwsh tools\s3.ps1 get  hare/<id>/wanxiang.userdb.txt
#   pwsh tools\s3.ps1 put  hare/<id>/wanxiang.userdb.txt local-file.txt
#   pwsh tools\s3.ps1 delete hare/<id>/wanxiang.userdb.txt
#   pwsh tools\s3.ps1 purge hare/          # delete everything under a prefix

param(
    [Parameter(Mandatory = $true)][ValidateSet('list', 'get', 'put', 'delete', 'purge')]
    [string]$Command,
    [string]$Key,
    [string]$File,
    [string]$Endpoint = $env:HARE_S3_ENDPOINT,
    [string]$Bucket = $env:HARE_S3_BUCKET,
    [string]$AccessKey = $env:HARE_S3_KEY,
    [string]$Secret = $env:HARE_S3_SECRET,
    [string]$Region = 'auto'
)

$ErrorActionPreference = 'Stop'
foreach ($required in @('Endpoint', 'Bucket', 'AccessKey', 'Secret')) {
    if (-not (Get-Variable $required -ValueOnly)) {
        throw "missing -$required (or the matching HARE_S3_* environment variable)"
    }
}
$s3Host = ([Uri]$Endpoint).Host

function Get-Hmac([byte[]]$key, [string]$message) {
    $hmac = New-Object System.Security.Cryptography.HMACSHA256
    $hmac.Key = $key
    $hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes($message))
}
function ConvertTo-Hex([byte[]]$bytes) { ($bytes | ForEach-Object { $_.ToString('x2') }) -join '' }
function Get-Sha256Hex([byte[]]$bytes) {
    ConvertTo-Hex ([Security.Cryptography.SHA256]::Create().ComputeHash($bytes))
}

function Invoke-S3([string]$method, [string]$uri, [string]$query, [byte[]]$payload) {
    if ($null -eq $payload) { $payload = [byte[]]@() }
    $now = [DateTime]::UtcNow
    $stamp = $now.ToString('yyyyMMddTHHmmssZ')
    $date = $now.ToString('yyyyMMdd')
    $payloadHash = Get-Sha256Hex $payload

    $canonical = "$method`n$uri`n$query`nhost:$s3Host`nx-amz-content-sha256:$payloadHash`nx-amz-date:$stamp`n`nhost;x-amz-content-sha256;x-amz-date`n$payloadHash"
    $scope = "$date/$Region/s3/aws4_request"
    $toSign = "AWS4-HMAC-SHA256`n$stamp`n$scope`n$(Get-Sha256Hex ([Text.Encoding]::UTF8.GetBytes($canonical)))"

    $signingKey = Get-Hmac ([Text.Encoding]::UTF8.GetBytes("AWS4$Secret")) $date
    $signingKey = Get-Hmac $signingKey $Region
    $signingKey = Get-Hmac $signingKey 's3'
    $signingKey = Get-Hmac $signingKey 'aws4_request'
    $signature = ConvertTo-Hex (Get-Hmac $signingKey $toSign)

    $auth = "AWS4-HMAC-SHA256 Credential=$AccessKey/$scope, SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature=$signature"
    $url = "$Endpoint$uri"
    if ($query) { $url += "?$query" }

    $args = @('-s', '-X', $method,
        '-H', "Authorization: $auth",
        '-H', "x-amz-content-sha256: $payloadHash",
        '-H', "x-amz-date: $stamp")
    if ($payload.Length -gt 0) {
        $temp = [IO.Path]::GetTempFileName()
        [IO.File]::WriteAllBytes($temp, $payload)
        $args += @('--data-binary', "@$temp")
    }
    $args += $url
    try { & curl.exe @args } finally { if ($temp) { Remove-Item $temp -Force -ErrorAction SilentlyContinue } }
}

function Get-Keys([string]$prefix) {
    $query = 'list-type=2'
    if ($prefix) { $query += "&prefix=$([Uri]::EscapeDataString($prefix))" }
    $xml = Invoke-S3 'GET' "/$Bucket" $query $null
    [regex]::Matches($xml, '<Key>([^<]+)</Key>') | ForEach-Object { $_.Groups[1].Value }
}

switch ($Command) {
    'list' { Get-Keys $Key }
    'get' { Invoke-S3 'GET' "/$Bucket/$Key" '' $null }
    'put' { Invoke-S3 'PUT' "/$Bucket/$Key" '' ([IO.File]::ReadAllBytes($File)); "put $Key" }
    'delete' { Invoke-S3 'DELETE' "/$Bucket/$Key" '' $null; "deleted $Key" }
    'purge' {
        $keys = Get-Keys $Key
        foreach ($k in $keys) { Invoke-S3 'DELETE' "/$Bucket/$k" '' $null | Out-Null }
        "deleted $($keys.Count) object(s)"
    }
}
