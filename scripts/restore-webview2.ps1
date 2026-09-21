# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.

[CmdletBinding()]
param([string]$CacheDir = '')

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$pin = Get-Content -LiteralPath (Join-Path $repo 'packaging/windows/webview2-sdk.json') -Raw | ConvertFrom-Json
if (-not $CacheDir) { $CacheDir = Join-Path $repo 'build-deps/webview2' }
$CacheDir = [IO.Path]::GetFullPath($CacheDir)
New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null
$package = Join-Path $CacheDir "microsoft.web.webview2.$($pin.version).nupkg"
if (-not (Test-Path -LiteralPath $package)) {
    $partial = "$package.download"
    try {
        Invoke-WebRequest -Uri $pin.url -OutFile $partial -UseBasicParsing
        if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pin.sha256) {
            throw 'WebView2 SDK archive hash does not match the pinned Microsoft package'
        }
        Move-Item -LiteralPath $partial -Destination $package
    } finally {
        if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
    }
}
if ((Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pin.sha256) {
    throw "Cached WebView2 SDK archive failed hash verification: $package"
}

$sdk = Join-Path $CacheDir $pin.version
$files = @('build/native/include/WebView2.h', 'build/native/include/WebView2EnvironmentOptions.h', 'build/native/x64/WebView2Loader.dll', 'LICENSE.txt', 'NOTICE.txt')
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($package)
try {
    foreach ($relative in $files) {
        $entry = $archive.GetEntry($relative)
        if (-not $entry) { throw "Required WebView2 SDK file is missing: $relative" }
        $destination = Join-Path $sdk $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        $entryStream = $entry.Open()
        try {
            $output = [IO.File]::Create($destination)
            try { $entryStream.CopyTo($output) } finally { $output.Dispose() }
        } finally { $entryStream.Dispose() }
    }
} finally { $archive.Dispose() }
@{
    version = $pin.version
    package_sha256 = $pin.sha256
    files = @($files | ForEach-Object {
        @{ name = $_; sha256 = (Get-FileHash -LiteralPath (Join-Path $sdk $_) -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $sdk 'sdk-manifest.json') -Encoding utf8
Write-Output $sdk
