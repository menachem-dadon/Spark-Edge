# MoE expert paging

## Configuration

`--moe-paging explicit` enables the subsystem. Set `--moe-cache-mib N`, `--moe-slots N`, or both. If both are present, both limits apply. The cache rejects a byte budget too small to hold at least the model's active-expert count per paged layer.

Additional options:

- `--moe-io-threads N`: file-read worker count.
- `--moe-io-mode buffered`: the implemented I/O path.
- `--moe-cache-policy lru`: the implemented policy.
- `--moe-paging-layers N`: page only the first N indexed layers; later layers stay resident.

Direct I/O is rejected because correct cross-platform alignment and tail handling are not implemented. MTP contexts are rejected in explicit mode.

## Memory accounting

Each indexed layer allocates compact three-dimensional tensor pools whose third dimension is the slot count. The byte budget is divided across indexed layer positions and converted using the exact tensor strides reported by GGML. Metadata, fixed weights, compute buffers, KV cache, and OS/runtime overhead are separate from this budget.

## Large microbatches

If one routing operation requests more unique experts than available slots, the request fails with the requested and available counts. Automatic graph microbatch splitting is not implemented in this alpha; use a smaller `--ubatch-size` or more slots.

## Split GGUF

Descriptors retain the upstream shard index and a duplicated read handle for each opened GGUF. Bounds are checked against each shard before inference begins.
