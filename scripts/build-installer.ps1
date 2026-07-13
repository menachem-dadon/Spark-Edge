# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 GiantLLM contributors.

[CmdletBinding()]
param(
    [string]$StageDir = '',
    [string]$OutputDir = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $repo 'VERSION') -Raw).Trim()
if (-not $OutputDir) { $OutputDir = Join-Path $repo 'artifacts/release' }
if (-not $StageDir) { $StageDir = Join-Path $OutputDir "giantllm-$version-windows-x64-avx2" }
if (-not (Test-Path (Join-Path $StageDir 'GiantLLM.exe'))) { throw "Portable stage is incomplete: $StageDir" }
$iscc = Get-Command ISCC.exe -ErrorAction SilentlyContinue
if (-not $iscc) {
    $candidate = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
    if (Test-Path $candidate) { $iscc = [pscustomobject]@{ Source = $candidate } }
}
if (-not $iscc) { throw 'Inno Setup 6 compiler (ISCC.exe) was not found.' }
$resolvedOutput = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force -Path $resolvedOutput | Out-Null
& $iscc.Source "/DSourceDir=$((Resolve-Path $StageDir).Path)" "/DOutputDir=$resolvedOutput" "/DAppVersion=$version" (Join-Path $repo 'packaging/windows/GiantLLM.iss')
if ($LASTEXITCODE) { throw "Inno Setup failed with exit code $LASTEXITCODE" }
$prefix = "giantllm-$version-"
$files = Get-ChildItem -LiteralPath $OutputDir -File | Where-Object {
    $_.Name.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and $_.Extension -in @('.zip', '.exe')
} | Sort-Object Name
$lines = foreach ($file in $files) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash.ToLower()
    "$hash  $($file.Name)"
}
$lines | Set-Content -Encoding ascii (Join-Path $OutputDir 'SHA256SUMS')
$files.FullName
