# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.

[CmdletBinding()]
param(
    [ValidateSet('avx2', 'generic')][string]$Variant = 'avx2',
    [ValidateSet('cpu', 'cuda', 'vulkan', 'hip', 'sycl')][string]$Backend = 'cpu',
    [string]$BuildRoot = '',
    [switch]$RunTests,
    [switch]$BrowserOnly,
    [switch]$EngineOnly,
    [ValidateRange(1, 64)][int]$Jobs = 2
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildRoot) {
    $suffix = if ($Backend -eq 'cpu') { $Variant } else { "$Variant-$Backend" }
    $BuildRoot = Join-Path $repo "build-windows-$suffix"
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $unicodeEnvironment = @{}
    Get-ChildItem Env: | Where-Object { $_.Value -match '[^\x00-\x7F]' } | ForEach-Object { $unicodeEnvironment[$_.Name] = $_.Value }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vsPath = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    if (-not $vsPath) { throw 'Visual Studio C++ Build Tools are required' }
    Import-Module (Join-Path $vsPath 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    $changedProfile = $env:USERPROFILE
    foreach ($name in $unicodeEnvironment.Keys) {
        if ($name -ne 'PATH') { [Environment]::SetEnvironmentVariable($name, $unicodeEnvironment[$name], 'Process') }
    }
    if ($changedProfile -ne $env:USERPROFILE) { $env:PATH = $env:PATH.Replace($changedProfile, $env:USERPROFILE) }
}

$cmakeRepo = $repo
$cmakeBuildRoot = $BuildRoot
if ($env:OS -eq 'Windows_NT') {
    New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
    $shortRepo = (& cmd.exe /d /c "for %I in (`"$repo`") do @echo %~sI").Trim()
    $shortBuildRoot = (& cmd.exe /d /c "for %I in (`"$BuildRoot`") do @echo %~sI").Trim()
    if ($shortRepo) { $cmakeRepo = $shortRepo }
    if ($shortBuildRoot) { $cmakeBuildRoot = $shortBuildRoot }
}

if (-not $EngineOnly) {
    $npm = Get-Command npm.cmd -ErrorAction SilentlyContinue
    if (-not $npm) { throw 'Node.js 22 and npm are required to build the Spark Edge chat UI' }
    $npmPath = $npm.Source
    if ($env:NODE_TLS_REJECT_UNAUTHORIZED -eq '0') { Remove-Item Env:NODE_TLS_REJECT_UNAUTHORIZED }
    $env:NODE_USE_SYSTEM_CA = '1'
    Push-Location (Join-Path $repo 'tools/ui')
    try {
        & $npmPath ci
        if ($LASTEXITCODE) { throw "npm ci failed with exit code $LASTEXITCODE" }
        & $npmPath run check
        if ($LASTEXITCODE) { throw "Chat UI check failed with exit code $LASTEXITCODE" }
        & $npmPath run build
        if ($LASTEXITCODE) { throw "Chat UI build failed with exit code $LASTEXITCODE" }
    } finally {
        Pop-Location
    }
}

$cmakeArgs = @(
    '-S', $cmakeRepo, '-B', $cmakeBuildRoot,
    '-DCMAKE_BUILD_TYPE=Release',
    '-DGGML_NATIVE=OFF',
    '-DLLAMA_BUILD_UI=OFF', '-DLLAMA_USE_PREBUILT_UI=OFF',
    '-DLLAMA_BUILD_SERVER=ON',
    '-DLLAMA_BUILD_TESTS=ON'
)
if ($EngineOnly) {
    $cmakeArgs += @('-DLLAMA_BUILD_EXAMPLES=OFF', '-DLLAMA_BUILD_APP=OFF', '-DSPARKMOE_WEBVIEW2=OFF')
} elseif ($BrowserOnly) {
    $cmakeArgs += '-DSPARKMOE_WEBVIEW2=OFF'
} else {
    $webviewSdk = & (Join-Path $PSScriptRoot 'restore-webview2.ps1')
    $cmakeArgs += @('-DSPARKMOE_WEBVIEW2=ON', "-DSPARKMOE_WEBVIEW2_SDK_DIR=$webviewSdk")
}
if ($Variant -eq 'generic') {
    $cmakeArgs += @('-DGGML_AVX=OFF', '-DGGML_AVX2=OFF', '-DGGML_FMA=OFF', '-DGGML_F16C=OFF', '-DGGML_BMI2=OFF')
} else {
    $cmakeArgs += @('-DGGML_AVX=ON', '-DGGML_AVX2=ON')
}

switch ($Backend) {
    'cuda' { $cmakeArgs += '-DGGML_CUDA=ON' }
    'vulkan' { $cmakeArgs += '-DGGML_VULKAN=ON' }
    'hip' { $cmakeArgs += @('-DGGML_HIP=ON', '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++') }
    'sycl' { $cmakeArgs += @('-DGGML_SYCL=ON', '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=icx') }
}
$upstream = (Get-Content -LiteralPath (Join-Path $repo 'UPSTREAM_BASE.txt') -Raw).Trim()
if ($upstream -notmatch '^[0-9a-f]{40}$') { throw 'UPSTREAM_BASE.txt does not contain a full commit SHA' }
$cmakeArgs += "-DLLAMA_BUILD_COMMIT=$upstream"
if (Get-Command ninja -ErrorAction SilentlyContinue) { $cmakeArgs += @('-G', 'Ninja') }
if ($Backend -eq 'vulkan' -and -not $env:GGML_VULKAN_SHADER_JOBS) {
    $env:GGML_VULKAN_SHADER_JOBS = [math]::Min($Jobs, 16).ToString()
}
& cmake @cmakeArgs
if ($LASTEXITCODE) { throw "CMake configure failed with exit code $LASTEXITCODE" }
$buildArgs = @('--build', $cmakeBuildRoot, '--config', 'Release', '--parallel', $Jobs)
if ($EngineOnly) {
    $buildArgs += @('--target', 'llama-server')
    if ($RunTests) { $buildArgs += @('test-moe-regression', 'test-moe-paging') }
}
& cmake @buildArgs
if ($LASTEXITCODE) { throw "Build failed with exit code $LASTEXITCODE" }
if ($RunTests) {
    & ctest --test-dir $cmakeBuildRoot -C Release --output-on-failure -R '^test-moe-'
    if ($LASTEXITCODE) { throw "Tests failed with exit code $LASTEXITCODE" }
    $bin = Join-Path $BuildRoot 'bin'
    if (Test-Path (Join-Path $bin 'Release')) { $bin = Join-Path $bin 'Release' }
    $smokeArgs = @('--bin-dir', $bin, '--log-dir', (Join-Path $BuildRoot 'server-smoke'))
    if ($EngineOnly) { $smokeArgs += '--skip-manager' }
    & python (Join-Path $PSScriptRoot 'test-sparkmoe-server.py') @smokeArgs
    if ($LASTEXITCODE) { throw "Server regression failed with exit code $LASTEXITCODE" }
}
