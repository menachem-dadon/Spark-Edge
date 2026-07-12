[CmdletBinding()]
param(
    [ValidateSet('avx2', 'generic')][string]$Variant = 'avx2',
    [string]$BuildRoot = '',
    [switch]$RunTests
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildRoot) { $BuildRoot = Join-Path $repo "build-windows-$Variant" }

$cmakeArgs = @(
    '-S', $repo, '-B', $BuildRoot,
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
& cmake --build $BuildRoot --config Release --target QwenStream llama-cli llama-server llama-bench qwenstream-iobench test-moe-paging --parallel
if ($LASTEXITCODE) { throw "Build failed with exit code $LASTEXITCODE" }
if ($RunTests) {
    & ctest --test-dir $BuildRoot -C Release --output-on-failure -R moe-paging
    if ($LASTEXITCODE) { throw "Tests failed with exit code $LASTEXITCODE" }
}
