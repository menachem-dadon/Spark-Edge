# Troubleshooting

## Incompatible expert layout

Explicit mode recognizes standard routed gate, up, down, and fused gate-up `.weight` tensors. If the model has another layout, loading stops before inference. Use `--moe-paging os` and report the exact architecture, GGUF metadata, and tensor layout before attempting support.

## MTP fails to initialize

The GGUF probably does not contain a compatible MTP head, or its MTP tensors do not match the target model. Disable MTP and confirm normal inference first. MTP is optional and can be slower even when it initializes correctly.

## Chat Server settings cannot reach the manager

Start `QwenStream.exe` and open the chat through its control center. The Server settings panel connects only to `127.0.0.1:18080` and only from the configured loopback chat origin. A server started directly from the CLI has no manager process to restart it.

## Cache budget is too small

The model activates more experts than the configured layer budget can hold. Increase `--moe-cache-mib` or `--moe-slots`. The error reports the minimum bytes when a byte budget is the limiting factor.

## Too many unique experts in a microbatch

Current builds automatically reduce the effective physical microbatch so that the maximum number of active experts fits in the per-layer slot cache. The adjustment is reported in the server log and as `effective_ubatch_size` from `GET /qwenstream/v1/config`.

If this message appears with an older binary, install the latest build or start the server with a smaller `--ubatch-size` value. `--ubatch-size 1` is the safest diagnostic setting, although prompt processing will be slower. Increasing the cache budget can also create more slots. QwenStream never evicts a slot reserved by the same routing request.

## Read timeout or short read

Verify all shards, file sizes, SSD health, and permissions. Network drives and removable media are not recommended. The failing layer/read is reported and inference stops.

## Launcher cannot find the server

Keep `QwenStream.exe` and `llama-server.exe` in the same folder. On MinGW packages, keep all bundled runtime DLLs beside the executables.

## WebUI build fails in a Unicode path

Use an ASCII drive alias for the build directory. QwenStream release builds require the customized source UI and should not publish an upstream prebuilt fallback. Runtime GGUF paths may still be Unicode.
