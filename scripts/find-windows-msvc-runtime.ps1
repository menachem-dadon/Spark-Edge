# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { return }

$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { return }

$redistRoot = Join-Path $vsInstall 'VC/Redist/MSVC'
if (-not (Test-Path -LiteralPath $redistRoot)) { return }

$crt = Get-ChildItem -LiteralPath $redistRoot -Directory |
    Where-Object Name -Match '^\d+\.' |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64/Microsoft.VC143.CRT' } |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1
if (-not $crt) { return }

Get-ChildItem -LiteralPath $crt -Filter '*.dll' -File
$openMp = Join-Path (Split-Path -Parent $crt) 'Microsoft.VC143.OpenMP'
if (Test-Path -LiteralPath $openMp) {
    Get-ChildItem -LiteralPath $openMp -Filter '*.dll' -File
}
