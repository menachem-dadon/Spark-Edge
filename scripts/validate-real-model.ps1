[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Model,
    [string]$BinDir = "$PSScriptRoot/../build-windows-avx2/bin",
    [int]$CacheMiB = 1536
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Model -PathType Leaf)) { throw "Model not found: $Model" }
$cli = Join-Path $BinDir 'llama-cli.exe'
if (-not (Test-Path $cli)) { throw "llama-cli not found: $cli" }
$prompt = 'Reply with a concise sentence in English and then in Hebrew.'
$baseArgs = @('-m', $Model, '--seed', '42', '--temp', '0', '-n', '128', '-p', $prompt, '--no-display-prompt', '--log-disable')
$resident = & $cli @baseArgs --moe-paging off 2>&1 | Out-String
$paged = & $cli @baseArgs --moe-paging explicit --moe-cache-mib $CacheMiB --moe-io-threads 3 2>&1 | Out-String
$equal = $resident -ceq $paged
$result = [ordered]@{
    completed_utc = (Get-Date).ToUniversalTime().ToString('o')
    model_file = [IO.Path]::GetFileName($Model)
    model_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $Model).Hash.ToLower()
    tokens = 128
    deterministic_output_equal = $equal
    resident_output_sha256 = ([BitConverter]::ToString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($resident)))).Replace('-', '').ToLower()
    paged_output_sha256 = ([BitConverter]::ToString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($paged)))).Replace('-', '').ToLower()
}
$path = Join-Path (Split-Path -Parent $PSScriptRoot) 'artifacts/real-model-validation.json'
New-Item -ItemType Directory -Force -Path (Split-Path $path) | Out-Null
$result | ConvertTo-Json | Set-Content -Encoding utf8 $path
if (-not $equal) { throw "Resident and paged deterministic outputs differ. See $path" }
Write-Output $path
