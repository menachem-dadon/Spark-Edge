[CmdletBinding()]
param([ValidateSet('rebase', 'merge')][string]$Strategy = 'rebase')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    $branch = (git branch --show-current).Trim()
    if (-not $branch) { throw 'Run this script on a named branch.' }
    git fetch upstream
    $backup = "qwenstream-backup/$((Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss'))"
    git branch $backup
    Write-Host "Backup branch: $backup"
    if ($Strategy -eq 'rebase') { git rebase upstream/master } else { git merge --no-ff upstream/master }
    if ($LASTEXITCODE) { throw 'Upstream integration stopped; resolve conflicts and continue or abort with Git.' }
    git rev-parse upstream/master | Set-Content -Encoding ascii UPSTREAM_BASE.txt
    & (Join-Path $PSScriptRoot 'build-windows.ps1') -Variant avx2 -RunTests
} finally { Pop-Location }
