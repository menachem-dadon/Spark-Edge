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
