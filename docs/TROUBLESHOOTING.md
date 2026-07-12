# Troubleshooting

## Unsupported architecture

Explicit mode currently accepts `qwen35moe` only. Use `--moe-paging os` for other architectures.

## Cache budget is too small

The model activates more experts than the configured layer budget can hold. Increase `--moe-cache-mib` or `--moe-slots`. The error reports the minimum bytes when a byte budget is the limiting factor.

## Too many unique experts in a microbatch

Reduce `--ubatch-size` or increase slots. QwenStream never evicts a slot reserved by the same routing request.

## Read timeout or short read

Verify all shards, file sizes, SSD health, and permissions. Network drives and removable media are not recommended. The failing layer/read is reported and inference stops.

## Launcher cannot find the server

Keep `QwenStream.exe` and `llama-server.exe` in the same folder. On MinGW packages, keep all bundled runtime DLLs beside the executables.

## WebUI build fails in a Unicode path

Use an ASCII drive alias for the build directory or allow the upstream verified UI archive fallback. Runtime GGUF paths may still be Unicode.
