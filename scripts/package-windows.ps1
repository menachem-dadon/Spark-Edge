[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDir,
    [ValidateSet('avx2', 'generic')][string]$Variant = 'avx2',
    [string]$OutputDir = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutputDir) { $OutputDir = Join-Path $repo 'artifacts/release' }
$version = (Get-Content (Join-Path $repo 'VERSION') -Raw).Trim()
$name = "qwenstream-$version-windows-x64-$Variant"
$stage = Join-Path $OutputDir $name
$bin = Join-Path $BuildDir 'bin'
if (-not (Test-Path (Join-Path $bin 'QwenStream.exe'))) { $bin = Join-Path $bin 'Release' }
$required = @('QwenStream.exe', 'llama-server.exe', 'llama-cli.exe', 'llama-bench.exe', 'qwenstream-iobench.exe')

New-Item -ItemType Directory -Force -Path $stage, (Join-Path $stage 'config'), (Join-Path $stage 'licenses'), (Join-Path $stage 'webui') | Out-Null
foreach ($file in $required) {
    $source = Join-Path $bin $file
    if (-not (Test-Path $source)) { throw "Missing release binary: $source" }
    Copy-Item -LiteralPath $source -Destination $stage
}

foreach ($dll in @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll', 'libgomp-1.dll', 'libdl.dll')) {
    $found = Get-Command $dll -ErrorAction SilentlyContinue
    if ($found) { Copy-Item -LiteralPath $found.Source -Destination $stage }
}
Copy-Item -Recurse -Force -LiteralPath (Join-Path $repo 'config/presets') -Destination (Join-Path $stage 'config')
Copy-Item -Force -LiteralPath (Join-Path $repo 'packaging/webui/README.txt') -Destination (Join-Path $stage 'webui')
Copy-Item -Force -LiteralPath (Join-Path $repo 'LICENSE'), (Join-Path $repo 'NOTICE'), (Join-Path $repo 'THIRD_PARTY_NOTICES.md') -Destination (Join-Path $stage 'licenses')
Copy-Item -Force -LiteralPath (Join-Path $repo 'README.md') -Destination (Join-Path $stage 'README.txt')
Copy-Item -Force -LiteralPath (Join-Path $repo 'README_HE.md') -Destination (Join-Path $stage 'README_HE.txt')
Copy-Item -Force -LiteralPath (Join-Path $repo 'VERSION') -Destination $stage

$zip = Join-Path $OutputDir "$name.zip"
if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
$hash = Get-FileHash -Algorithm SHA256 -LiteralPath $zip
"$($hash.Hash.ToLower())  $([IO.Path]::GetFileName($zip))" | Set-Content -Encoding ascii (Join-Path $OutputDir 'SHA256SUMS')
Write-Output $zip
