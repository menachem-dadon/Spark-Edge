# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.

[CmdletBinding()]
param(
    [string]$StageDir = '',
    [string]$OutputDir = '',
    [string]$WebView2Bootstrapper = '',
    [ValidatePattern('^$|^[A-Fa-f0-9]{40}$')][string]$SigningCertificateThumbprint = '',
    [string]$TimestampServer = 'http://timestamp.digicert.com'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $repo 'VERSION') -Raw).Trim()
if (-not $OutputDir) { $OutputDir = Join-Path $repo 'artifacts/release' }
if (-not $StageDir) { $StageDir = Join-Path $OutputDir "spark-edge-$version-windows-x64-avx2" }
if (-not (Test-Path (Join-Path $StageDir 'SparkMoE.exe'))) { throw "Portable stage is incomplete: $StageDir" }
$manifest = Get-Content -LiteralPath (Join-Path $StageDir 'release-manifest.json') -Raw | ConvertFrom-Json
$variant = $manifest.variant
if ($variant -notin @('avx2', 'generic')) { throw 'The portable release manifest has no valid CPU variant' }
foreach ($entry in $manifest.files) {
    if ((Get-FileHash -LiteralPath (Join-Path $StageDir $entry.name) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
        throw "Portable stage differs from its manifest: $($entry.name)"
    }
}
$bootstrapperInfo = $null
$compilerArgs = @("/DSourceDir=$((Resolve-Path $StageDir).Path)", "/DAppVersion=$version", "/DVariant=$variant")
if ($manifest.desktop.enabled) {
    if (-not (Test-Path -LiteralPath (Join-Path $StageDir 'WebView2Loader.dll'))) { throw 'The portable desktop stage is missing WebView2Loader.dll' }
    if (-not $WebView2Bootstrapper) {
        $WebView2Bootstrapper = Join-Path $repo 'build-deps/webview2/MicrosoftEdgeWebview2Setup.exe'
        if (-not (Test-Path -LiteralPath $WebView2Bootstrapper)) {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $WebView2Bootstrapper) | Out-Null
            Invoke-WebRequest -Uri 'https://go.microsoft.com/fwlink/p/?LinkId=2124703' -OutFile $WebView2Bootstrapper -UseBasicParsing
        }
    }
    $bootstrapperSignature = Get-AuthenticodeSignature -LiteralPath $WebView2Bootstrapper
    if ($bootstrapperSignature.Status -ne 'Valid' -or $bootstrapperSignature.SignerCertificate.Subject -notmatch '(^|,\s*)CN=Microsoft Corporation(,|$)') {
        throw 'The WebView2 bootstrapper must have a valid Microsoft Corporation Authenticode signature'
    }
    $compilerArgs += "/DWebView2Bootstrapper=$((Resolve-Path -LiteralPath $WebView2Bootstrapper).Path)"
    $bootstrapperInfo = @{
        name = 'MicrosoftEdgeWebview2Setup.exe'
        version = (Get-Item -LiteralPath $WebView2Bootstrapper).VersionInfo.FileVersion
        sha256 = (Get-FileHash -LiteralPath $WebView2Bootstrapper -Algorithm SHA256).Hash.ToLowerInvariant()
        signature = $bootstrapperSignature.Status.ToString()
    }
}
$signingCertificate = $null
if ($SigningCertificateThumbprint) {
    $signingCertificate = Get-Item -LiteralPath "Cert:\CurrentUser\My\$SigningCertificateThumbprint"
    if (-not $signingCertificate.HasPrivateKey -or $signingCertificate.NotAfter -le (Get-Date)) {
        throw 'The selected code-signing certificate needs an available private key and must not be expired'
    }
}
$iscc = Get-Command ISCC.exe -ErrorAction SilentlyContinue
if (-not $iscc) {
    $candidate = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
    if (Test-Path $candidate) { $iscc = [pscustomobject]@{ Source = $candidate } }
}
if (-not $iscc) { throw 'Inno Setup 6 compiler (ISCC.exe) was not found.' }
$resolvedOutput = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force -Path $resolvedOutput | Out-Null
$compilerArgs += "/DOutputDir=$resolvedOutput"
& $iscc.Source @compilerArgs (Join-Path $repo 'packaging/windows/SparkMoE.iss')
if ($LASTEXITCODE) { throw "Inno Setup failed with exit code $LASTEXITCODE" }
$installer = Join-Path $resolvedOutput "spark-edge-$version-windows-x64-$variant-setup.exe"
if ($signingCertificate) {
    $signature = Set-AuthenticodeSignature -LiteralPath $installer -Certificate $signingCertificate -HashAlgorithm SHA256 -TimestampServer $TimestampServer
    if ($signature.Status -ne 'Valid' -or -not $signature.TimeStamperCertificate) {
        throw 'Installer signing or trusted timestamp verification failed'
    }
}
@{
    name = [IO.Path]::GetFileName($installer)
    sha256 = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
    signature = (Get-AuthenticodeSignature -LiteralPath $installer).Status.ToString()
    webview2_bootstrapper = $bootstrapperInfo
} | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath "$installer.manifest.json" -Encoding utf8
$prefix = "spark-edge-$version-"
$files = Get-ChildItem -LiteralPath $OutputDir -File | Where-Object {
    $_.Name.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and $_.Extension -in @('.zip', '.exe')
} | Sort-Object Name
$lines = foreach ($file in $files) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash.ToLower()
    "$hash  $($file.Name)"
}
$lines | Set-Content -Encoding ascii (Join-Path $OutputDir 'SHA256SUMS')
$files.FullName
