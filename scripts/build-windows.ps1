[CmdletBinding()]
param(
    [ValidateSet('avx2', 'generic')][string]$Variant = 'avx2',
    [string]$BuildRoot = '',
    [switch]$RunTests
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildRoot) { $BuildRoot = Join-Path $repo "build-windows-$Variant" }

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
if (-not $npm) { throw 'Node.js 22 and npm are required to build the QwenStream chat UI' }
$npmPath = $npm.Source
Push-Location (Join-Path $repo 'tools/ui')
try {
    & $npmPath ci
    if ($LASTEXITCODE) { throw "npm ci failed with exit code $LASTEXITCODE" }
    & $npmPath run build
    if ($LASTEXITCODE) { throw "Chat UI build failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}

$cmakeArgs = @(
    '-S', $cmakeRepo, '-B', $cmakeBuildRoot,
    '-DCMAKE_BUILD_TYPE=Release',
    '-DGGML_NATIVE=OFF',
    '-DLLAMA_CURL=OFF',
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
& cmake --build $cmakeBuildRoot --config Release --target QwenStream llama-cli llama-server llama-bench qwenstream-iobench test-moe-paging --parallel
if ($LASTEXITCODE) { throw "Build failed with exit code $LASTEXITCODE" }
if ($RunTests) {
    & ctest --test-dir $cmakeBuildRoot -C Release --output-on-failure -R moe-paging
    if ($LASTEXITCODE) { throw "Tests failed with exit code $LASTEXITCODE" }
}
