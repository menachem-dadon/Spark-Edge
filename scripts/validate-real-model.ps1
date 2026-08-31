# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Model,
    [string]$BinDir = "$PSScriptRoot/../build-windows-avx2/bin",
    [string]$OutputDir = "$PSScriptRoot/../artifacts/real-model-validation"
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Model -PathType Leaf)) { throw "Model not found: $Model" }
$test = Join-Path $BinDir 'test-moe-regression.exe'
if (-not (Test-Path -LiteralPath $test)) { throw "Build the native regression target first: $test" }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$log = Join-Path $OutputDir 'full-logits.log'
# Both sides use identical CPU kernels and microbatch. A small quantized MoE is recommended.
# The resident reference must fit this machine; this command never downloads a model.
& $test --model $Model *> $log
$code = $LASTEXITCODE
@{
    completed_utc = (Get-Date).ToUniversalTime().ToString('o')
    model_file = [IO.Path]::GetFileName($Model)
    model_bytes = (Get-Item -LiteralPath $Model).Length
    result = $(if ($code -eq 0) { 'PASS' } else { 'FAIL' })
    exit_code = $code
    log = [IO.Path]::GetFullPath($log)
    comparison = 'Full vocabulary logits; fixed teacher-forced tokens, CPU kernels and microbatch; 512 MiB paging cache'
} | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $OutputDir 'result.json')
if ($code) { throw "Real-model full-logit regression failed ($code). See $log" }
Write-Output $log
