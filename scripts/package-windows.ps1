# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDir,
    [ValidateSet('avx2', 'generic')][string]$Variant = 'avx2',
    [string]$OutputDir = '',
    [string]$UiNodeModulesDir = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutputDir) { $OutputDir = Join-Path $repo 'artifacts/release' }
$version = (Get-Content (Join-Path $repo 'VERSION') -Raw).Trim()
$name = "spark-edge-$version-windows-x64-$Variant"
$stage = Join-Path $OutputDir $name
$outputRoot = [IO.Path]::GetFullPath($OutputDir).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$stageRoot = [IO.Path]::GetFullPath($stage)
if (-not $stageRoot.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to stage outside the release output directory: $stageRoot"
}
if (Test-Path $stageRoot) { Remove-Item -LiteralPath $stageRoot -Recurse -Force }
$bin = Join-Path $BuildDir 'bin'
if (-not (Test-Path (Join-Path $bin 'SparkMoE.exe'))) { $bin = Join-Path $bin 'Release' }
$required = @('SparkMoE.exe', 'llama-server.exe', 'llama-cli.exe', 'llama-bench.exe', 'sparkmoe-iobench.exe')
$cmakeCache = Get-Content -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') -Raw -Encoding UTF8
$webviewEnabled = $cmakeCache -match '(?m)^SPARKMOE_WEBVIEW2:BOOL=ON\r?$'
$webviewManifest = $null
if ($webviewEnabled) {
    $required += 'WebView2Loader.dll'
    $sdkMatch = [regex]::Match($cmakeCache, '(?m)^SPARKMOE_WEBVIEW2_SDK_DIR:PATH=([^\r\n]+)')
    if (-not $sdkMatch.Success) { throw 'The WebView2 SDK directory is missing from the build cache' }
    $webviewSdk = $sdkMatch.Groups[1].Value
    $pin = Get-Content -LiteralPath (Join-Path $repo 'packaging/windows/webview2-sdk.json') -Raw | ConvertFrom-Json
    $webviewManifest = Get-Content -LiteralPath (Join-Path $webviewSdk 'sdk-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($webviewManifest.version -ne $pin.version -or $webviewManifest.package_sha256 -ne $pin.sha256) {
        throw 'The WebView2 SDK manifest differs from the pinned version; run scripts/restore-webview2.ps1 and rebuild'
    }
    foreach ($entry in $webviewManifest.files) {
        if ((Get-FileHash -LiteralPath (Join-Path $webviewSdk $entry.name) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
            throw "WebView2 SDK file failed hash verification: $($entry.name)"
        }
    }
    $loaderHash = ($webviewManifest.files | Where-Object { $_.name -eq 'build/native/x64/WebView2Loader.dll' }).sha256
    if ((Get-FileHash -LiteralPath (Join-Path $bin 'WebView2Loader.dll') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $loaderHash) {
        throw 'The built WebView2 loader differs from the restored SDK; rebuild Spark Edge before packaging'
    }
}

New-Item -ItemType Directory -Force -Path $stage, (Join-Path $stage 'assets'), (Join-Path $stage 'config'), (Join-Path $stage 'docs'), (Join-Path $stage 'licenses'), (Join-Path $stage 'webui') | Out-Null
foreach ($file in $required) {
    $source = Join-Path $bin $file
    if (-not (Test-Path $source)) { throw "Missing release binary: $source" }
    Copy-Item -LiteralPath $source -Destination $stage
}

Get-ChildItem -LiteralPath $bin -Filter '*.dll' -File | Where-Object {
    $webviewEnabled -or $_.Name -ne 'WebView2Loader.dll'
} | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $stage
}
$buildInfo = Join-Path $BuildDir 'common/build-info.cpp'
$buildInfoText = if (Test-Path -LiteralPath $buildInfo) { Get-Content -LiteralPath $buildInfo -Raw } else { '' }
$isMsvcBuild = $buildInfoText -match 'LLAMA_COMPILER = "MSVC'
if ($isMsvcBuild) {
    & (Join-Path $PSScriptRoot 'find-windows-msvc-runtime.ps1') | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $stage
    }
    if (-not (Test-Path (Join-Path $stage 'vcruntime140.dll'))) {
        throw 'The MSVC runtime must be staged from the Visual Studio redistributable directory'
    }
    if ((Get-Content -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') -Raw) -match 'GGML_OPENMP:BOOL=ON' -and
            -not (Test-Path (Join-Path $stage 'vcomp140.dll'))) {
        throw 'The MSVC OpenMP runtime vcomp140.dll is required by this build'
    }
}

if (-not $isMsvcBuild) {
    foreach ($dll in @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll', 'libgomp-1.dll', 'libdl.dll')) {
        $found = Get-Command $dll -ErrorAction SilentlyContinue
        if ($found) { Copy-Item -LiteralPath $found.Source -Destination $stage }
    }
}
Copy-Item -Recurse -Force -LiteralPath (Join-Path $repo 'config/presets') -Destination (Join-Path $stage 'config')
Copy-Item -Recurse -Force -LiteralPath (Join-Path $repo 'assets/branding') -Destination (Join-Path $stage 'assets')
Copy-Item -Force -LiteralPath (Join-Path $repo 'packaging/webui/README.txt') -Destination (Join-Path $stage 'webui')
Copy-Item -Force -LiteralPath (Join-Path $repo 'LICENSE'), (Join-Path $repo 'LICENSE-SPARKMOE-NONCOMMERCIAL.md'), (Join-Path $repo 'LICENSING.md'), (Join-Path $repo 'NOTICE'), (Join-Path $repo 'THIRD_PARTY_NOTICES.md') -Destination (Join-Path $stage 'licenses')
if ($webviewEnabled) {
    $notices = Join-Path $stage 'licenses/WebView2'
    New-Item -ItemType Directory -Force -Path $notices | Out-Null
    Copy-Item -LiteralPath (Join-Path $webviewSdk 'LICENSE.txt'), (Join-Path $webviewSdk 'NOTICE.txt'), (Join-Path $webviewSdk 'sdk-manifest.json') -Destination $notices
}
Copy-Item -LiteralPath (Join-Path $repo 'packaging/windows/PORTABLE.txt') -Destination (Join-Path $stage 'PORTABLE.txt')
foreach ($relative in (& git -C $repo ls-files '*LICENSE*' '*COPYING*')) {
    if ($relative -notmatch '^(vendor|ggml|common|tools)/') { continue }
    $target = Join-Path (Join-Path $stage 'licenses/upstream') $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo $relative) -Destination $target
}
if (-not $UiNodeModulesDir) { $UiNodeModulesDir = Join-Path $repo 'tools/ui/node_modules' }
if (-not (Test-Path -LiteralPath $UiNodeModulesDir)) { throw 'Build the chat UI first so its dependency notices can be staged' }
$uiModules = (Resolve-Path -LiteralPath $UiNodeModulesDir).Path
Get-ChildItem -LiteralPath $uiModules -File -Recurse | Where-Object {
    $_.Name -match '^(LICENSE|LICENCE|COPYING|NOTICE)([.-]|$)'
} | ForEach-Object {
    $relative = $_.FullName.Substring($uiModules.Length).TrimStart([char[]]'\/')
    $target = Join-Path (Join-Path $stage 'licenses/chat-ui') $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $target
}
Copy-Item -Force -LiteralPath (Join-Path $repo 'README.md') -Destination (Join-Path $stage 'README.txt')
Copy-Item -Force -LiteralPath (Join-Path $repo 'README_HE.md') -Destination (Join-Path $stage 'README_HE.txt')
Copy-Item -Force -LiteralPath (Join-Path $repo 'VERSION') -Destination $stage
Copy-Item -Force -LiteralPath (Join-Path $repo 'docs/USER_GUIDE.md'), (Join-Path $repo 'docs/TROUBLESHOOTING.md'), (Join-Path $repo 'docs/PROJECT_MAP.md'), (Join-Path $repo 'docs/SUPPORTED_MODELS.md') -Destination (Join-Path $stage 'docs')
foreach ($document in @('ENGINE_UPGRADES.md', 'UPSTREAM_INTEGRATION.md', 'UPSTREAM_PATCH_INVENTORY.md', 'UPDATING_LLAMA_CPP.md', 'REFRESH_REPORT.md', 'LATENCY_INVESTIGATION.md', 'NEXT_SESSION.md', 'QWEN38_IMPLEMENTATION.md', 'WEBVIEW2_DESKTOP_INTEGRATION_PLAN.md', 'INTEGRATIONS.md', 'UX_RELEASE_EXECUTION.md')) {
    $source = Join-Path $repo "docs/$document"
    if (Test-Path $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $stage 'docs') }
}
$binaryCommit = if (Test-Path -LiteralPath $buildInfo) {
    $match = [regex]::Match($buildInfoText, 'LLAMA_COMMIT = "([^"]+)"')
    if ($match.Success) { $match.Groups[1].Value }
}
@{
    version = $version
    variant = $Variant
    source_revision = (& git -C $repo rev-parse HEAD)
    source_tree_dirty = [bool](& git -C $repo status --porcelain)
    binary_build_commit = $binaryCommit
    desktop = @{
        enabled = $webviewEnabled
        sdk_version = if ($webviewEnabled) { $webviewManifest.version } else { $null }
        sdk_package_sha256 = if ($webviewEnabled) { $webviewManifest.package_sha256 } else { $null }
        runtime = if ($webviewEnabled) { 'Microsoft WebView2 Evergreen, installed separately' } else { 'Default browser' }
    }
    ui_package_lock_sha256 = (Get-FileHash -LiteralPath (Join-Path (Split-Path -Parent $uiModules) 'package-lock.json') -Algorithm SHA256).Hash.ToLower()
    upstream_revision = (Get-Content (Join-Path $repo 'UPSTREAM_BASE.txt') -Raw).Trim()
    files = @(Get-ChildItem -LiteralPath $stage -File | Where-Object { $_.Extension -in @('.exe', '.dll') } | ForEach-Object {
        @{ name = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower(); signature = (Get-AuthenticodeSignature -LiteralPath $_.FullName).Status.ToString() }
    })
} | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $stage 'release-manifest.json')

$zip = Join-Path $OutputDir "$name.zip"
if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
$hash = Get-FileHash -Algorithm SHA256 -LiteralPath $zip
"$($hash.Hash.ToLower())  $([IO.Path]::GetFileName($zip))" | Set-Content -Encoding ascii (Join-Path $OutputDir 'SHA256SUMS')
Write-Output $zip
