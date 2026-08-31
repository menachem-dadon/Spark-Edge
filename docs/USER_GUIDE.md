# User guide

Use the launcher for the simplest local workflow. It listens only on `127.0.0.1:18080`, stores form preferences in browser local storage, scans the selected models folder for `.gguf` files, validates the chosen model, and starts the sibling `llama-server` without a command shell.

The RAM bar shows current system memory use and available memory. It is not a model loading estimate and does not prove that a particular model fits.

For a split model, select the first conventional shard; upstream discovers the remaining shards. Keep all shards together and do not rename only one file.

Keep `auto` mode for the normal workflow. It attempts bounded expert streaming when the selected GGUF has a compatible routed MoE layout, and retries with standard mmap loading when the model is dense or incompatible. Qwen3.5/Qwen3.6 MoE and Gemma 4 26B-A4B use the standard routed layout. Reduce context or microbatch size before reducing the expert cache below the model's required active-expert minimum.

The launcher selects conservative context, microbatch, expert cache, and SSD worker defaults from the machine's RAM and CPU count. These are starting values, not a promise that a particular model fits. A user can still select Standard (`os`), explicit expert streaming, or fully resident loading.

Advanced loading settings expose expert slots, inference CPU threads, and CPU weight repacking. Leave threads at `0` and repacking on `Automatic` unless benchmarking or diagnosing a specific model. `Disabled / compatibility` maps to `--no-repack` and is an escape hatch for a CPU kernel or quantization problem; it is not the normal performance setting.

Context length, full-context behavior, and MTP are changed under **Chat settings -> Context & MTP**. The common controls are shown directly; batching, KV precision, and other tuning fields stay in a collapsed advanced section. Applying them restarts the inference server and reloads the model. The chat page waits for the new server and reconnects automatically.

Enable MTP only when the GGUF includes a compatible MTP head. Start with three draft tokens and measure throughput with the same prompt both on and off; MTP is not faster on every system.

The launcher does not download models. Upstream CLI/server Hugging Face options remain available, but large downloads should be deliberate and model licenses must be reviewed first.

## Advanced engine controls

Keep buffered I/O and per-layer LRU initially. Direct I/O uses aligned native reads
with byte validation and buffered fallback; it can be slower when the OS cache
is warm. Hybrid shares a budget across layers while retaining layer floors and
manual expert pinning. Hybrid, overlap and temporal prefetch currently require
CPU-only inference. None changes routing, top-k or expert weight bytes.

The hybrid resize button queues the cache MiB setting for the next decode without
restarting the server. Accepted does not mean applied: watch pending/failure and
committed/limit metrics. Pinned experts, layer floors and speculative rollback
requirements can prevent a shrink. MTP owns a separate draft cache; the button
resizes only the main context.

Dense/shared residency controls the ordinary upstream loader: automatic, mmap,
allocated resident memory or requested OS locking. This policy also applies to
unpaged expert layers. OS locking may fail due to platform/privilege limits.
First N paged layers means model-layer numbers, including any initial dense
layers; 0 selects all routed layers.

For recurrent MTP, effective microbatch must be at least draft tokens + 2. The
expert cache must hold every selected expert for that microbatch. A too-small
configuration is rejected; increase microbatch and cache/slots or disable MTP.
Never reduce model top-k to fit memory. Runtime resize preserves this minimum.

Cache RAM is separate from whole-process RSS/private memory and system RAM.
Displayed tok/s is the cumulative generation average from llama-server metrics.
Stall/queue counters are cumulative observations, not independent percentages.
Compare options with the same model, context, threads and prompt; see
`ENGINE_UPGRADES.md` for measured limitations.
