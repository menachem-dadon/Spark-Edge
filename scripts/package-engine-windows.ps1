# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$BuildDir,
    [ValidateSet('cpu', 'cuda', 'vulkan', 'hip', 'sycl')][string]$Backend = 'cpu',
    [string]$OutputDir = 'artifacts/engines',
    [string[]]$RuntimeLibraries = @()
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$commit = (Get-Content -LiteralPath (Join-Path $repo 'UPSTREAM_BASE.txt') -Raw).Trim()
if ($commit -notmatch '^[0-9a-f]{40}$') { throw 'The upstream revision must be a full SHA' }
$cache = Join-Path $BuildDir 'CMakeCache.txt'
$recordedCommit = Select-String -LiteralPath $cache -Pattern "^LLAMA_BUILD_COMMIT:[^=]+=$commit$"
if (-not $recordedCommit) { throw 'Build the engine from the upstream revision recorded in UPSTREAM_BASE.txt' }
$expectedOption = @{cuda='GGML_CUDA'; hip='GGML_HIP'; vulkan='GGML_VULKAN'; sycl='GGML_SYCL'}
foreach ($kind in $expectedOption.Keys) {
    $option = $expectedOption[$kind]
    if ($kind -eq $Backend) {
        if (-not (Select-String -LiteralPath $cache -Pattern "^$($option):BOOL=ON$")) { throw "Missing build option $option" }
    } elseif (Select-String -LiteralPath $cache -Pattern "^$($option):BOOL=ON$") {
        throw "The engine contains an unexpected backend $kind"
    }
}

$bin = Join-Path $BuildDir 'bin'
if (-not (Test-Path -LiteralPath (Join-Path $bin 'llama-server.exe'))) { $bin = Join-Path $bin 'Release' }
$server = Get-Item -LiteralPath (Join-Path $bin 'llama-server.exe')
$files = @($server) + @(Get-ChildItem -LiteralPath $bin -Filter '*.dll' -File | Where-Object Name -ne 'WebView2Loader.dll')
foreach ($library in $RuntimeLibraries) { $files += Get-Item -LiteralPath $library }

$buildInfo = Join-Path $BuildDir 'common/build-info.cpp'
if ((Test-Path -LiteralPath $buildInfo) -and
        (Get-Content -LiteralPath $buildInfo -Raw) -match 'LLAMA_COMPILER = "MSVC') {
    $runtime = & (Join-Path $PSScriptRoot 'find-windows-msvc-runtime.ps1')
    $files += @($runtime | Where-Object { $_ -and $_.Name -notin $files.Name })
    if ('vcruntime140.dll' -notin $files.Name) { throw 'The MSVC runtime is required in the engine package' }
    if ((Select-String -LiteralPath $cache -Pattern '^GGML_OPENMP:BOOL=ON$') -and
            'vcomp140.dll' -notin $files.Name) {
        throw 'The MSVC OpenMP runtime is required in the engine package'
    }
}
if (($files.Name | Sort-Object -Unique).Count -ne $files.Count) { throw 'Duplicate runtime DLL names' }

$id = "$Backend-$commit"
$prefix = "spark-edge-engine-win-x64-$id"
New-Item -Path $OutputDir -ItemType Directory -Force | Out-Null
$entries = foreach ($file in $files) {
    if ($file.Name -ne 'llama-server.exe' -and $file.Name -notmatch '^[A-Za-z0-9][A-Za-z0-9_-]{0,79}\.dll$') {
        throw "Unexpected binary name $($file.Name)"
    }
    $asset = Join-Path $OutputDir "$prefix-$($file.Name)"
    Copy-Item -LiteralPath $file.FullName -Destination $asset -Force
    @{
        name = $file.Name
        sha256 = (Get-FileHash -LiteralPath $asset -Algorithm SHA256).Hash.ToLowerInvariant()
        size = $file.Length
    }
}
$manifest = @{ schema = 1; router_api = 2; id = $id; files = @($entries) } | ConvertTo-Json -Depth 4
[IO.File]::WriteAllText((Join-Path (Resolve-Path -LiteralPath $OutputDir) "$prefix.json"), $manifest + [Environment]::NewLine)
