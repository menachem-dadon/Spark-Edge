# Troubleshooting

## Unsupported architecture

Explicit mode currently accepts `qwen35moe` only. Use `--moe-paging os` for other architectures.

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

Use an ASCII drive alias for the build directory or allow the upstream verified UI archive fallback. Runtime GGUF paths may still be Unicode.
