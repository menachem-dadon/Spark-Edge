# Architecture

SparkMoE keeps the upstream execution path intact unless explicit paging is requested for a model with a validated standard routed-expert tensor layout.

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

The hidden local manager listens on `127.0.0.1:18080` and launches the sibling `llama-server` as a Router on the user-selected stable Web/API port. The Router owns one child process per loaded model and enforces the user-selected `models-max` limit with idle LRU eviction. SparkMoE writes a global default and per-model overrides to an INI preset consumed by the existing llama.cpp Router.

On Windows, the manager and all child processes are created without console windows. The manager captures the Router's combined stdout/stderr; the Router already folds each model child's output into that stream. Every start/restart creates a persistent session log under the user's local application data. The embedded SparkMoE page reads management state and logs from port 18080, while inference, model lifecycle, and SparkMoE runtime metrics use the normal Router port.

Loading, paging, context, KV cache, MTP, and default reasoning are process-profile settings. OpenAI-compatible messages, tools, sampling, output length, and thinking controls remain request-level inputs and are proxied unchanged to the selected model.

## Platform layer

Windows code is isolated in `moe-io-windows.cpp` and uses duplicated handles plus overlapped `ReadFile`. POSIX code uses positional reads. The cache and model index contain no direct platform API calls.
