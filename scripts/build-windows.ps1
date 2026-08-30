# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.

[CmdletBinding()]
param(
    [ValidateSet('avx2', 'generic')][string]$Variant = 'avx2',
    [string]$BuildRoot = '',
    [switch]$RunTests
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildRoot) { $BuildRoot = Join-Path $repo "build-windows-$Variant" }
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vsPath = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    if (-not $vsPath) { throw 'Visual Studio C++ Build Tools are required' }
    Import-Module (Join-Path $vsPath 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
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

$npm = Get-Command npm.cmd -ErrorAction SilentlyContinue
if (-not $npm) { throw 'Node.js 22 and npm are required to build the SparkMoE chat UI' }
$npmPath = $npm.Source
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

$cmakeArgs = @(
    '-S', $cmakeRepo, '-B', $cmakeBuildRoot,
    '-DCMAKE_BUILD_TYPE=Release',
    '-DGGML_NATIVE=OFF',
    '-DLLAMA_BUILD_UI=OFF', '-DLLAMA_USE_PREBUILT_UI=OFF',
    '-DLLAMA_BUILD_SERVER=ON',
    '-DLLAMA_BUILD_TESTS=ON'
)
if ($Variant -eq 'generic') {
    $cmakeArgs += @('-DGGML_AVX=OFF', '-DGGML_AVX2=OFF', '-DGGML_FMA=OFF', '-DGGML_F16C=OFF', '-DGGML_BMI2=OFF')
} else {
    $cmakeArgs += @('-DGGML_AVX=ON', '-DGGML_AVX2=ON')
}

if (Get-Command ninja -ErrorAction SilentlyContinue) { $cmakeArgs += @('-G', 'Ninja') }
& cmake @cmakeArgs
if ($LASTEXITCODE) { throw "CMake configure failed with exit code $LASTEXITCODE" }
& cmake --build $cmakeBuildRoot --config Release --parallel 4
if ($LASTEXITCODE) { throw "Build failed with exit code $LASTEXITCODE" }
if ($RunTests) {
    & ctest --test-dir $cmakeBuildRoot -C Release --output-on-failure -R '^test-moe-'
    if ($LASTEXITCODE) { throw "Tests failed with exit code $LASTEXITCODE" }
    $bin = Join-Path $BuildRoot 'bin'
    if (Test-Path (Join-Path $bin 'Release')) { $bin = Join-Path $bin 'Release' }
    & python (Join-Path $PSScriptRoot 'test-sparkmoe-server.py') --bin-dir $bin --log-dir (Join-Path $BuildRoot 'server-smoke')
    if ($LASTEXITCODE) { throw "Server regression failed with exit code $LASTEXITCODE" }
}
