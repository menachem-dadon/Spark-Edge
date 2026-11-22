# User guide

Run `SparkMoE.exe` for the normal local workflow. On Windows it is a GUI subsystem process, so no command window remains open. Desktop builds open the chat in a Spark Edge window using Microsoft WebView2, with automatic browser fallback if the Runtime is unavailable. The manager serves the application at `http://127.0.0.1:18080/`, so the interface remains available when inference stops. The management API remains loopback-only.

Open **Spark Edge** in the llama.cpp sidebar. The **Models** tab contains the model library, downloads, and settings for the selected model. **Connections** configures external clients, and **Activity** contains runtime measurements, logs, and diagnostics. Shared chat Settings also exposes the selected model's Spark Edge controls.

Add a model file or folder to the library. For a persistent discovery folder, the selected path remains a draft until **Save folder** is pressed; background status refreshes do not replace that draft. **Rescan saved folder** refreshes the model list without restarting the Router. The scan is recursive. A split GGUF appears once through its `00001` shard, and a vision `mmproj` or MTP/draft sidecar in the same directory is attached to its model instead of appearing as a separate model. The normal llama.cpp model picker can then load and unload those models. `models-max` controls simultaneous model processes: `1` is conservative for large models, higher values are useful for small models, and `0` is unlimited. When the limit is reached, the Router evicts the least recently used idle process.

Each model has a persistent Spark Edge profile. It controls startup-time values such as paging mode, expert cache, context, batches, KV cache types, MTP, threads, and the default reasoning mode. Saving preserves the profile without interrupting current requests. Apply saved changes explicitly to reload that model; startup settings also take effect on its next load.

Keep `auto` mode for the normal workflow. It attempts bounded expert streaming when the selected GGUF has a compatible routed MoE layout, and retries with standard mmap loading when the model is dense or incompatible. The hardware-derived defaults are starting values, not a promise that a particular model fits.

The selected model's settings contain context length, context shift, parallel slots, batches, prompt-cache RAM, checkpoints, and K/V precision. Advanced controls include speculative decoding, the model's default reasoning effort (`Default`, `Off`, `Low`, `Medium`, `High`, or `Max`), and optional MTP sidecar, draft, cache, thread, and sampling controls. Enable MTP only when the GGUF includes a compatible MTP head or when a matching sidecar GGUF is configured.

The **Activity** tab's **Logs and diagnostics** section shows the current log tail, previous sessions, and the privacy-filtered diagnostic export. The full combined Router and child-model output is stored under the user's local Spark Edge data directory and can be downloaded without truncation from the UI. Spark Edge does not write prompts or generations into its own management records.

**Stop** stops the Router and loaded models while the application remains available. **Exit Spark Edge** stops the Router, every model process, and the manager itself. Closing the desktop window exits by default; enabling background operation keeps the notification-area controls available. Closing with an active proxied generation asks for confirmation. Select the notification-area icon to reopen the interface, or use its menu to start, stop, and exit. Launching the same shortcut again activates the existing window. Browser fallback retains the same manager controls.

The desktop browser profile, settings, and logs are stored under `%LOCALAPPDATA%\SparkMoE` by default and survive upgrades and uninstall. Browser and desktop conversations use separate browser storage; transfer them with the chat export/import controls. See `PORTABLE.txt` in the Windows package for browser recovery and an explicit custom data-folder option.

The Models tab offers explicit Hugging Face GGUF downloads with progress and pause/resume controls. Downloaded models are added to the library after validation. Upstream CLI/server Hugging Face options remain available. Choose downloads deliberately and review the model license before use.

## External API behavior

Use the local OpenAI-compatible URL shown in Connections, normally `http://127.0.0.1:18080/v1/chat/completions`. Every routed request must name the model. With automatic loading enabled, an unloaded model starts with its saved Spark Edge profile before the request is forwarded. With automatic loading disabled, load it first from the Web UI or `POST /models/load`.

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
