param(
    [Parameter(Mandatory = $true)][string]$Model,
    [Parameter(Mandatory = $true)][string]$DraftModel,
    [Parameter(Mandatory = $true)][string]$Server,
    [ValidateRange(1, 65536)][int]$ExpertCacheMiB = 3072,
    [ValidateRange(1, 4096)][int]$PleCacheMiB = 64,
    [ValidateRange(1, 65536)][int]$DraftCacheMiB = 512,
    [ValidateRange(1, 64)][int]$DraftWidth = 1,
    [ValidateRange(128, 1048576)][int]$Context = 4096,
    [ValidateRange(1, 128)][int]$Threads = 4,
    [ValidateRange(1, 128)][int]$IoThreads = 4,
    [ValidateRange(1024, 65535)][int]$Port = 8080
)

foreach ($path in @($Model, $DraftModel, $Server)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required file does not exist: $path"
    }
}

$serverArgs = @(
    '--model', (Resolve-Path -LiteralPath $Model).Path,
    '--model-draft', (Resolve-Path -LiteralPath $DraftModel).Path,
    '--host', '127.0.0.1', '--port', $Port,
    '--parallel', '1', '--ctx-size', $Context,
    '--batch-size', '128', '--ubatch-size', '2',
    '--threads', $Threads, '--threads-batch', $Threads,
    '--n-gpu-layers', '0', '--fit', 'off', '--no-warmup', '--no-context-shift',
    '--moe-paging', 'explicit', '--moe-cache-policy', 'hybrid',
    '--moe-cache-mib', $ExpertCacheMiB, '--moe-ple-cache-mib', $PleCacheMiB,
    '--moe-io-mode', 'buffered', '--moe-io-threads', $IoThreads,
    '--moe-overlap', 'off', '--moe-prefetch', 'off', '--moe-dense-residency', 'resident',
    '--spec-type', 'draft-mtp', '--spec-draft-n-max', $DraftWidth,
    '--spec-draft-p-min', '0.75', '--moe-draft-cache-mib', $DraftCacheMiB,
    '--metrics'
)

& (Resolve-Path -LiteralPath $Server).Path @serverArgs
exit $LASTEXITCODE
