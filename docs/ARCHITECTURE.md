# Architecture

QwenStream keeps the upstream execution path intact unless explicit paging is requested for a model with a validated standard routed-expert tensor layout.

## Load path

The normal GGUF loader reads metadata for every shard. Routed expert tensors are duplicated only as no-allocation tensor metadata and recorded in `model_index` with shard, 64-bit file offset, byte length, type, shape, stride, role, layer, and expert count. Other tensors follow normal llama.cpp allocation or mapping.

## Token path

1. The upstream Qwen graph computes router scores and selected expert IDs.
2. A CPU custom graph operation asks the per-layer cache to resolve those IDs.
3. Hits update LRU state. Misses reserve non-active slots and submit each expert tensor component to the I/O worker pool.
4. The cache waits on condition/future completion, rejects errors, and publishes the original-expert-to-slot mapping.
5. Slot IDs feed the existing `MUL_MAT_ID` kernels. Original IDs remain in use for per-expert scales and LoRA side data.

There is no zero-weight fallback or replacement expert. An invalid ID, short read, timeout, missing shard, incompatible layout, or insufficient slot count fails explicitly.

## Concurrency

Cache state is mutex-protected and counters are atomic. Explicit mode currently requires `--parallel 1` because cross-sequence slot lifetimes have not completed real-model validation. The MTP target and draft contexts own separate cache managers, so their slot lifetimes do not collide. OS and resident modes may use additional server slots through normal llama.cpp behavior.

## Model compatibility

The model loader pages only standard gate, up, down, and fused gate-up routed expert `.weight` tensors. It validates their expert axis, stride, layer, file offset, and shard bounds. All other tensors remain on the normal upstream path. This structural policy supports Qwen, Gemma 4, and other compatible MoE families without silently claiming that every architecture has been real-model validated.

## Control plane

The local manager listens on `127.0.0.1:18080`, validates configuration, launches the sibling inference server, tails its local log, and proxies only QwenStream status endpoints. Cross-origin access is restricted to the manager and currently configured loopback chat origin. Context and MTP changes use a stop/start cycle because their memory is allocated during model initialization.

## Platform layer

Windows code is isolated in `moe-io-windows.cpp` and uses duplicated handles plus overlapped `ReadFile`. POSIX code uses positional reads. The cache and model index contain no direct platform API calls.
