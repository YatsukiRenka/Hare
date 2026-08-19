# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Yatsuki Renka

# Minimal S3 client for inspecting a sync bucket during development.
# The secret is accepted as a SecureString or prompted for; nothing is stored here.
#
#   $env:HARE_S3_ENDPOINT / HARE_S3_BUCKET / HARE_S3_KEY / HARE_S3_PREFIX
#
#   pwsh tools\s3.ps1 list
#   pwsh tools\s3.ps1 get  hare/<id>/wanxiang.userdb.txt recovered.bin
#   pwsh tools\s3.ps1 put  hare/<id>/wanxiang.userdb.txt local-file.txt
#   pwsh tools\s3.ps1 delete hare/<id>/wanxiang.userdb.txt
#   pwsh tools\s3.ps1 purge hare/          # delete an explicit prefix
#   pwsh tools\s3.ps1 purge other/ -All    # explicitly allow another prefix

param(
    [Parameter(Mandatory = $true)][ValidateSet('list', 'get', 'put', 'delete', 'purge')]
    [string]$Command,
    [string]$Key,
    [string]$File,
    [string]$Endpoint = $env:HARE_S3_ENDPOINT,
    [string]$Bucket = $env:HARE_S3_BUCKET,
    [string]$AccessKey = $env:HARE_S3_KEY,
    [Security.SecureString]$Secret,
    [string]$Region = 'auto',
    [string]$Prefix = $(if ($env:HARE_S3_PREFIX) { $env:HARE_S3_PREFIX } else { 'hare/' }),
    [switch]$All
)

$ErrorActionPreference = 'Stop'
foreach ($required in @('Endpoint', 'Bucket', 'AccessKey')) {
    if (-not (Get-Variable $required -ValueOnly)) {
        throw "missing -$required (or the matching HARE_S3_* environment variable)"
    }
}
try {
    $endpointUri = [Uri]::new($Endpoint, [UriKind]::Absolute)
} catch {
    throw '-Endpoint must be an absolute HTTPS URL without a path, query, fragment, or userinfo'
}
$schemeEnd = $Endpoint.IndexOf('://', [StringComparison]::Ordinal)
$authorityStart = $schemeEnd + 3
$pathStart = if ($authorityStart -ge 3) { $Endpoint.IndexOf('/', $authorityStart) } else { -1 }
if (-not $endpointUri.IsAbsoluteUri -or $endpointUri.Scheme -ne 'https' -or
    -not $endpointUri.Host -or $schemeEnd -lt 1 -or
    $endpointUri.AbsolutePath -ne '/' -or $endpointUri.Query -or
    $endpointUri.Fragment -or $endpointUri.UserInfo -or
    $Endpoint.IndexOf('?') -ge 0 -or $Endpoint.IndexOf('#') -ge 0 -or
    $Endpoint.IndexOf('\', $authorityStart) -ge 0 -or
    ($pathStart -ge 0 -and $pathStart -ne $Endpoint.Length - 1)) {
    throw '-Endpoint must be an absolute HTTPS URL without a path, query, fragment, or userinfo'
}
$Endpoint = $endpointUri.GetLeftPart([UriPartial]::Authority)
$hostName = $endpointUri.IdnHost
if ($hostName.Contains(':') -and -not $hostName.StartsWith('[')) {
    $hostName = "[$hostName]"
}
$s3Host = if ($endpointUri.IsDefaultPort) {
    $hostName
} else {
    '{0}:{1}' -f $hostName, $endpointUri.Port
}

if ([string]::IsNullOrWhiteSpace($Prefix) -or -not $Prefix.EndsWith('/')) {
    throw '-Prefix must be non-empty and end in /'
}

switch ($Command) {
    'get' {
        if ([string]::IsNullOrWhiteSpace($Key)) { throw 'get needs -Key' }
        if ([string]::IsNullOrWhiteSpace($File)) { throw 'get needs -File' }
    }
    'put' {
        if ([string]::IsNullOrWhiteSpace($Key)) { throw 'put needs -Key' }
        if ([string]::IsNullOrWhiteSpace($File)) { throw 'put needs -File' }
    }
    'delete' {
        if ([string]::IsNullOrWhiteSpace($Key)) { throw 'delete needs -Key' }
        if (-not $All -and
            -not $Key.StartsWith($Prefix, [StringComparison]::Ordinal)) {
            throw "deleting outside $Prefix needs -All"
        }
    }
    'purge' {
        if ([string]::IsNullOrWhiteSpace($Key)) { throw 'purge needs -Key' }
        if (-not $Key.EndsWith('/')) {
            throw 'purge prefix must be non-empty and end in /'
        }
        if (-not $All -and
            -not $Key.StartsWith($Prefix, [StringComparison]::Ordinal)) {
            throw "purging outside $Prefix needs -All"
        }
    }
}

$ownsSecret = $false
if ($null -eq $Secret) {
    $Secret = Read-Host 'Secret' -AsSecureString
    $ownsSecret = $true
}
if ($Secret.Length -eq 0) {
    if ($ownsSecret) { $Secret.Dispose() }
    throw 'Secret must not be empty'
}

function Get-Hmac([byte[]]$key, [string]$message) {
    $hmac = [Security.Cryptography.HMACSHA256]::new($key)
    try {
        return ,$hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes($message))
    } finally {
        $hmac.Dispose()
    }
}
function ConvertTo-Hex([byte[]]$bytes) { ($bytes | ForEach-Object { $_.ToString('x2') }) -join '' }
function Get-Sha256Hex([byte[]]$bytes) {
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        ConvertTo-Hex $sha256.ComputeHash($bytes)
    } finally {
        $sha256.Dispose()
    }
}

function Get-SigningRootKey([Security.SecureString]$secure) {
    $bstr = [IntPtr]::Zero
    $characters = $null
    $plainBytes = $null
    try {
        $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
        $characters = [char[]]::new($secure.Length)
        for ($i = 0; $i -lt $characters.Length; ++$i) {
            $characters[$i] = [char][Runtime.InteropServices.Marshal]::ReadInt16(
                $bstr, $i * 2)
        }
        $plainBytes = [Text.Encoding]::UTF8.GetBytes($characters)
        $root = [byte[]]::new(4 + $plainBytes.Length)
        [Text.Encoding]::ASCII.GetBytes('AWS4').CopyTo($root, 0)
        [Array]::Copy($plainBytes, 0, $root, 4, $plainBytes.Length)
        return ,$root
    } finally {
        if ($null -ne $plainBytes) {
            [Array]::Clear($plainBytes, 0, $plainBytes.Length)
        }
        if ($null -ne $characters) {
            [Array]::Clear($characters, 0, $characters.Length)
        }
        if ($bstr -ne [IntPtr]::Zero) {
            [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
        }
    }
}

function ConvertTo-Rfc3986([string]$value) {
    $encoded = New-Object Text.StringBuilder
    foreach ($byte in [Text.Encoding]::UTF8.GetBytes($value)) {
        $unreserved =
            ($byte -ge [byte][char]'A' -and $byte -le [byte][char]'Z') -or
            ($byte -ge [byte][char]'a' -and $byte -le [byte][char]'z') -or
            ($byte -ge [byte][char]'0' -and $byte -le [byte][char]'9') -or
            $byte -in @([byte][char]'-', [byte][char]'.', [byte][char]'_', [byte][char]'~')
        if ($unreserved) {
            [void]$encoded.Append([char]$byte)
        } else {
            [void]$encoded.AppendFormat('%{0:X2}', $byte)
        }
    }
    $encoded.ToString()
}

# Encode each path segment once, then use the same result for signing and sending.
function Get-S3Uri([string]$key) {
    $segments = @($Bucket)
    if ($PSBoundParameters.ContainsKey('key')) {
        $segments += [regex]::Split($key, '/')
    }
    '/' + (($segments | ForEach-Object { ConvertTo-Rfc3986 $_ }) -join '/')
}

function Get-ListQuery([string]$prefix, [string]$continuationToken) {
    $parts = @()
    if ($continuationToken) {
        $parts += "continuation-token=$(ConvertTo-Rfc3986 $continuationToken)"
    }
    $parts += 'list-type=2'
    if ($prefix) {
        $parts += "prefix=$(ConvertTo-Rfc3986 $prefix)"
    }
    $parts -join '&'
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

    $signingKey = Get-Hmac $signingRootKey $date
    $signingKey = Get-Hmac $signingKey $Region
    $signingKey = Get-Hmac $signingKey 's3'
    $signingKey = Get-Hmac $signingKey 'aws4_request'
    $signature = ConvertTo-Hex (Get-Hmac $signingKey $toSign)

    $auth = "AWS4-HMAC-SHA256 Credential=$AccessKey/$scope, SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature=$signature"
    $url = "$Endpoint$uri"
    if ($query) { $url += "?$query" }

    $request = [Net.Http.HttpRequestMessage]::new(
        [Net.Http.HttpMethod]::new($method), $url)
    $response = $null
    try {
        $request.Headers.Host = $s3Host
        [void]$request.Headers.TryAddWithoutValidation('Authorization', $auth)
        [void]$request.Headers.TryAddWithoutValidation(
            'x-amz-content-sha256', $payloadHash)
        [void]$request.Headers.TryAddWithoutValidation('x-amz-date', $stamp)
        if ($method -eq 'PUT' -or $payload.Length -gt 0) {
            $request.Content = [Net.Http.ByteArrayContent]::new($payload)
        }

        try {
            $response = $httpClient.SendAsync(
                $request, [Net.Http.HttpCompletionOption]::ResponseContentRead
            ).GetAwaiter().GetResult()
        } catch {
            throw "S3 $method $uri failed: $($_.Exception.Message)"
        }
        $body = $response.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult()
        if (-not $response.IsSuccessStatusCode) {
            $statusCode = [int]$response.StatusCode
            $failure = "S3 $method $uri failed (HTTP $statusCode)"
            $errorBody = [Text.Encoding]::UTF8.GetString($body).Trim()
            if ($errorBody) { $failure += ": $errorBody" }
            throw $failure
        }
        return ,$body
    } finally {
        if ($null -ne $response) { $response.Dispose() }
        $request.Dispose()
    }
}

function Get-Keys([string]$prefix) {
    $continuationToken = ''
    do {
        $query = Get-ListQuery $prefix $continuationToken
        $responseBytes = Invoke-S3 'GET' (Get-S3Uri) $query $null
        [xml]$document = [Text.Encoding]::UTF8.GetString([byte[]]$responseBytes)
        $document.SelectNodes(
            "/*[local-name()='ListBucketResult']/*[local-name()='Contents']/*[local-name()='Key']") |
            ForEach-Object { $_.InnerText }

        $truncatedNode = $document.SelectSingleNode(
            "/*[local-name()='ListBucketResult']/*[local-name()='IsTruncated']")
        $truncated = $null -ne $truncatedNode -and $truncatedNode.InnerText.Trim() -eq 'true'
        if ($truncated) {
            $tokenNode = $document.SelectSingleNode(
                "/*[local-name()='ListBucketResult']/*[local-name()='NextContinuationToken']")
            if ($null -eq $tokenNode -or -not $tokenNode.InnerText) {
                throw 'S3 list response is truncated but has no continuation token'
            }
            $continuationToken = $tokenNode.InnerText
        }
    } while ($truncated)
}

$signingRootKey = $null
$handler = $null
$httpClient = $null
try {
    $signingRootKey = Get-SigningRootKey $Secret
    $handler = [Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $false
    $httpClient = [Net.Http.HttpClient]::new($handler, $true)

    switch ($Command) {
        'list' { Get-Keys $Key }
        'get' {
            $bytes = Invoke-S3 'GET' (Get-S3Uri $Key) '' $null
            [IO.File]::WriteAllBytes($File, [byte[]]$bytes)
            "wrote $File"
        }
        'put' {
            Invoke-S3 'PUT' (Get-S3Uri $Key) '' ([IO.File]::ReadAllBytes($File)) |
                Out-Null
            "put $Key"
        }
        'delete' {
            Invoke-S3 'DELETE' (Get-S3Uri $Key) '' $null | Out-Null
            "deleted $Key"
        }
        'purge' {
            $keys = @(Get-Keys $Key)
            foreach ($k in $keys) {
                if (-not $k.StartsWith($Key, [StringComparison]::Ordinal)) {
                    throw "S3 list returned a key outside purge prefix $Key"
                }
                Invoke-S3 'DELETE' (Get-S3Uri $k) '' $null | Out-Null
            }
            "deleted $($keys.Count) object(s)"
        }
    }
} finally {
    if ($null -ne $httpClient) { $httpClient.Dispose() }
    if ($null -ne $signingRootKey) {
        [Array]::Clear($signingRootKey, 0, $signingRootKey.Length)
    }
    if ($ownsSecret) { $Secret.Dispose() }
}
