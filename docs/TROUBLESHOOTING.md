# Troubleshooting

## Spark Edge opens in a browser instead of its own window

Keep `WebView2Loader.dll` beside `SparkMoE.exe` when extracting a desktop ZIP. The desktop window requires Microsoft's shared [Evergreen WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/). The Windows installer offers an optional Runtime installation task. Missing Runtime or native-window initialization failure opens browser fallback while the manager remains available. Check the session log for the `desktop:` stage and HRESULT.

For a recovery launch, set `SPARKMOE_BROWSER_ONLY=1` in the launching process. This skips WebView2 and retains the same manager interface at `http://127.0.0.1:18080/`. Do not delete the WebView2 profile to switch modes. Browser and desktop profiles are separate; use the chat export/import controls to transfer conversations.

## The server exits during the first prompt on Windows

Current Windows AVX2 builds avoid passing 256-bit lookup vectors across the MinGW function boundary used by the Q4_0, IQ4_NL, and MXFP4 repack kernels. Older builds could load a model successfully and then exit with `0xC0000005` only when inference used more than one worker thread. This was a CPU stack-alignment failure, not an out-of-memory condition or a damaged GGUF.

Install the current build first. If another CPU or quantization-specific failure remains, open Advanced loading settings and set CPU weight repacking to `Disabled / compatibility`, or pass `--no-repack`. Keep the model, context, and thread count unchanged when comparing logs so that the override is a useful diagnostic.

## Automatic loading retries as standard mmap

This is expected for a dense model or a routed layout that explicit expert streaming does not support. Automatic mode first uses the configured expert cache when possible and otherwise restarts model loading through normal mmap. Select explicit mode only when a hard failure is preferable to fallback during compatibility testing.

## Incompatible expert layout

Explicit mode recognizes standard routed gate, up, down, and fused gate-up `.weight` tensors. If the model has another layout, loading stops before inference. Use automatic mode for transparent standard mmap fallback, or `--moe-paging os` when standard loading must be forced. Report the exact architecture, GGUF metadata, and tensor layout before attempting support.

## MTP fails to initialize

The GGUF probably does not contain a compatible MTP head, or its MTP tensors do not match the target model. Disable MTP and confirm normal inference first. MTP is optional and can be slower even when it initializes correctly.

## The Spark Edge page cannot reach the manager

Start `SparkMoE.exe` rather than `llama-server.exe` directly. The Spark Edge page connects only to `127.0.0.1:18080` and only from the configured loopback Web UI origin. A server started directly from the CLI has no manager process, persistent profile store, or session-log API.

## Cache budget is too small

The model activates more experts than the configured layer budget can hold. Increase `--moe-paging-cache-mib` or `--moe-slots`. The error reports the minimum bytes when a byte budget is the limiting factor.

## Too many unique experts in a microbatch

For long zero-progress intervals after submitting short chat messages, see
[LATENCY_INVESTIGATION.md](LATENCY_INVESTIGATION.md). Chat token counts include
the template and history. Progress advances at logical batch boundaries;
microbatch size alone does not determine how often the UI updates. Version
0.4.1-alpha also fixes native log creation and Desktop log reading under Hebrew
Windows installation paths and enables timestamps. It does not automatically
retune the user's settings or claim a latency speedup.

Current builds automatically reduce the effective physical microbatch so that the maximum number of active experts fits in the per-layer slot cache. The adjustment is reported in the server log and as `effective_ubatch_size` from `GET /sparkmoe/v1/config`.

If this message appears with an older binary, install the latest build or start the server with a smaller `--ubatch-size` value. `--ubatch-size 1` is the safest diagnostic setting, although prompt processing will be slower. Increasing the cache budget can also create more slots. Spark Edge never evicts a slot reserved by the same routing request.

## Read timeout or short read

Verify all shards, file sizes, SSD health, and permissions. Network drives and removable media are not recommended. The failing layer/read is reported and inference stops.

## Launcher cannot find the server

Keep `SparkMoE.exe` and `llama-server.exe` in the same folder. On MinGW packages, keep all bundled runtime DLLs beside the executables.

## No CMD window appears

This is expected. The Windows launcher is a GUI process and starts the Router and model children with no console window. Open **Spark Edge -> Logs & diagnostics** in the Web UI for the same complete output. Full session files are stored under `%LOCALAPPDATA%\SparkMoE\logs`.

## WebUI build fails in a Unicode path

Use an ASCII drive alias for the build directory. Spark Edge release builds require the customized source UI and should not publish an upstream prebuilt fallback. Runtime GGUF paths may still be Unicode.
