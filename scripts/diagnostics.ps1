[CmdletBinding()]
param([string]$Output = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Output) { $Output = Join-Path $repo 'artifacts/qwenstream-diagnostics.zip' }
$temp = Join-Path ([IO.Path]::GetTempPath()) ("qwenstream-diagnostics-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1 Name, NumberOfCores, NumberOfLogicalProcessors
    $os = Get-CimInstance Win32_OperatingSystem
    [ordered]@{
        version = (Get-Content (Join-Path $repo 'VERSION') -Raw).Trim()
        upstream_commit = (Get-Content (Join-Path $repo 'UPSTREAM_BASE.txt') -Raw).Trim()
        os = $os.Caption
        os_version = $os.Version
        ram_mib = [math]::Round($os.TotalVisibleMemorySize / 1024)
        cpu = $cpu
        privacy = 'No API keys, HF tokens, prompts, generated text, usernames, or full personal paths are included.'
    } | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $temp 'system.json')
    if (Test-Path (Join-Path $repo 'artifacts/test-results')) { Copy-Item -Recurse (Join-Path $repo 'artifacts/test-results') $temp }
    New-Item -ItemType Directory -Force -Path (Split-Path $Output) | Out-Null
    Compress-Archive -Path (Join-Path $temp '*') -DestinationPath $Output -Force
} finally { Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue }
Write-Output $Output
