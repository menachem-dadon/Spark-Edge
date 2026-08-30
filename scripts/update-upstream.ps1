# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.
[CmdletBinding()]
param([string]$Target = 'latest', [string]$Resume = '')
$ErrorActionPreference = 'Stop'
$python = Get-Command python -ErrorAction Stop
$arguments = @((Join-Path $PSScriptRoot 'sparkmoe-upstream.py'))
if ($Resume) { $arguments += @('resume', '--repo', $Resume) }
else { $arguments += @('update', '--target', $Target) }
& $python.Source @arguments
exit $LASTEXITCODE
