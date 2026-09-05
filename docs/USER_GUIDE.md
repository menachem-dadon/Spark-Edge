# User guide

Run `SparkMoE.exe` for the normal local workflow. On Windows it is a GUI subsystem process, so no command window remains open. It starts `llama-server` in Router mode, waits for the Web UI, and opens the browser directly. The management API remains loopback-only on `127.0.0.1:18080`.

Open **SparkMoE** in the llama.cpp sidebar. This page uses the same UI system as the original chat and contains all fork-specific controls. Its seven tabs are **Overview**, **Models**, **SparkMoE engine**, **Context & caches**, **MTP & reasoning**, **Metrics**, and **Logs & diagnostics**. The rest of Settings and the chat routes retain the llama.cpp structure and behavior.

Choose a models folder and apply the Router settings. The selected path remains an unsaved draft until **Apply server settings** is pressed; background status refreshes do not replace that draft. **Rescan saved folder** refreshes the model list without restarting the Router. The scan is recursive. A split GGUF appears once through its `00001` shard, and a vision `mmproj` or MTP/draft sidecar in the same directory is attached to its model instead of appearing as a separate model. The normal llama.cpp model picker can then load and unload those models. `models-max` controls simultaneous model processes: `1` is conservative for large models, higher values are useful for small models, and `0` is unlimited. When the limit is reached, the Router evicts the least recently used idle process.

Each model has a persistent SparkMoE profile. It controls startup-time values such as paging mode, expert cache, context, batches, KV cache types, MTP, threads, and the default reasoning mode. Saving a loaded model's profile reloads that model only; other model processes remain available.

Keep `auto` mode for the normal workflow. It attempts bounded expert streaming when the selected GGUF has a compatible routed MoE layout, and retries with standard mmap loading when the model is dense or incompatible. The hardware-derived defaults are starting values, not a promise that a particular model fits.

The **Context & caches** tab owns context length, context shift, parallel slots, batches, prompt-cache RAM, checkpoints, and K/V precision. **MTP & reasoning** is separate: it owns the model's default reasoning effort (`Default`, `Off`, `Low`, `Medium`, `High`, or `Max`) and optional MTP sidecar, draft, cache, thread, and sampling controls. Enable MTP only when the GGUF includes a compatible MTP head or when a matching sidecar GGUF is configured.

The **Logs & diagnostics** tab shows the current log tail, previous sessions, and the privacy-filtered diagnostic export. The full combined Router and child-model output is stored under the user's local SparkMoE data directory and can be downloaded without truncation from the UI. SparkMoE does not write prompts or generations into its own management records.

**Stop** stops the Router and loaded models but keeps the hidden manager available so the server can be started again. **Exit SparkMoE** stops the Router, every model process, and the manager itself. On Windows, the SparkMoE notification-area icon remains visible while the manager is running. Select it to reopen the UI, or use its menu to inspect Router state, start or stop the Router, and exit the complete application. The tooltip reports whether the Router is running and its port. If the browser was closed while SparkMoE is still running, use the notification-area icon or launch the same Start-menu shortcut again; the single-instance launcher reopens the existing Web UI instead of starting another manager.

The launcher does not download models. Upstream CLI/server Hugging Face options remain available, but large downloads should be deliberate and model licenses must be reviewed first.

## External API behavior

Use the normal llama.cpp OpenAI-compatible URL, normally `http://127.0.0.1:8080/v1/chat/completions`. Every routed request must name the model. With automatic loading enabled, an unloaded model starts with its saved SparkMoE profile before the request is forwarded. With automatic loading disabled, load it first from the Web UI or `POST /models/load`.

Generation options do not come from the loading profile. The caller still controls messages, tools, sampling, output length, and thinking for each request. To disable thinking for one SmartiAI request, send both:

```json
{
  "model": "your-model-id",
  "messages": [{ "role": "user", "content": "Hello" }],
  "reasoning_effort": "none",
  "chat_template_kwargs": { "enable_thinking": false }
}
```

The model profile's reasoning level is only the server default. In the built-in chat, **Default** uses that profile; **Off**, **Low**, **Medium**, **High**, or **Max** sends a request-level override for that conversation. API callers can use `reasoning_effort` and `thinking_budget_tokens` in the same way. Request-level thinking changes do not reload the model or change context, paging, cache, MTP, or thread allocation.

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
