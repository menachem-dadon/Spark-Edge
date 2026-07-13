# Engineering decisions

## Current upstream base

The exact llama.cpp commit is recorded in `UPSTREAM_BASE.txt`; the official repository is configured as the `upstream` remote.

## Independent CPU implementation

The current subsystem follows modern Qwen graph and model-loader interfaces. It does not cherry-pick the older Metal proof of concept and does not copy Colibri implementation code.

## Exactness over fallback

Missing experts, invalid IDs, incompatible layouts, insufficient slots, I/O errors, and unsupported direct I/O are fatal configuration/runtime errors. QwenStream does not continue with approximate weights.

## Buffered I/O first

Windows overlapped buffered reads and POSIX positional reads provide a correct portable baseline. Direct I/O is rejected until alignment, shard tails, quantized layout, and fallback semantics can be validated.

## One parallel context by default

Explicit paging requires `--parallel 1` for predictable slot lifetimes, CPU memory, and I/O. OS and resident modes can use additional slots. MTP is safe because its draft context owns a separate cache manager.

## Structural MoE compatibility

Explicit paging recognizes standard routed expert weight roles and validates their real GGUF layout. This is safer and easier to maintain than a growing architecture-name switch while still rejecting models whose tensors cannot be paged exactly.

## Startup settings stay in the control plane

Context allocation and MTP draft contexts cannot be changed per completion request. The manager owns these settings and performs an explicit restart. The chat UI exposes the same manager controls rather than pretending they are request-time sampling options.

## Partial layer paging

`n_layers` pages only indexed layers in the requested prefix. All other layers remain on the normal resident path; the graph tests `handles_layer()` before binding a pool.
