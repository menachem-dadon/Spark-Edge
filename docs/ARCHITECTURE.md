# Architecture

QwenStream keeps the upstream execution path intact unless explicit paging is requested for a supported Qwen MoE model.

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

Cache state is mutex-protected and counters are atomic. QwenStream v0.1.0-alpha intentionally configures the launcher server with `--parallel 1`; a single llama context serializes decode work. Multi-context shared-cache residency and cross-context pin lifetimes are not implemented.

## Platform layer

Windows code is isolated in `moe-io-windows.cpp` and uses duplicated handles plus overlapped `ReadFile`. POSIX code uses positional reads. The cache and model index contain no direct platform API calls.
