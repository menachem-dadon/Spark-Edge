# Engineering decisions

## Current upstream base

The exact llama.cpp commit is recorded in `UPSTREAM_BASE.txt`; the official repository is configured as the `upstream` remote.

## Independent CPU implementation

The current subsystem follows modern Qwen graph and model-loader interfaces. It does not cherry-pick the older Metal proof of concept and does not copy Colibri implementation code.

## Exactness over fallback

Missing experts, invalid IDs, incompatible layouts, insufficient slots, I/O errors, and unsupported direct I/O are fatal configuration/runtime errors. QwenStream does not continue with approximate weights.

## Buffered I/O first

Windows overlapped buffered reads and POSIX positional reads provide a correct portable baseline. Direct I/O is rejected until alignment, shard tails, quantized layout, and fallback semantics can be validated.

## Single parallel context in the launcher

The alpha cache lifetime is scoped to one context. The launcher passes `--parallel 1` so a later request cannot overwrite a slot while an earlier context uses it.

## Partial layer paging

`n_layers` pages only indexed layers in the requested prefix. All other layers remain on the normal resident path; the graph tests `handles_layer()` before binding a pool.
