# QwenStream

QwenStream is an independent, CPU-first fork of `llama.cpp` that can keep routed Qwen3.5/Qwen3.6 MoE expert tensors on a GGUF file and load only the experts selected by the router into bounded per-layer RAM caches.

This is **v0.1.0-alpha**. The paging core, Windows asynchronous file reader, LRU cache, CLI/server integration, launcher, and synthetic deterministic tests are implemented and built. Full Qwen3.5/Qwen3.6 35B-A3B validation has not been completed because no full model was available locally. Do not treat this alpha as a performance or 16 GB compatibility guarantee.

## What it does

- Leaves non-routed weights on the normal `llama.cpp` path.
- Indexes routed expert tensor offsets across single or split GGUF files.
- Loads selected experts exactly, without zero weights, expert substitution, or approximation.
- Uses bounded per-layer LRU slot pools and asynchronous buffered file I/O.
- Exposes real cache/I/O statistics through C, `llama-server`, and `llama-bench`.
- Includes a local-only English/Hebrew launcher dashboard and SSD I/O benchmark.

It does not page GPU weights, support direct/unbuffered expert I/O, or promise that every Qwen quantization fits in 16 GB. Explicit mode currently supports the `qwen35moe` architecture only.

## Hardware

- Windows 10/11 x64 or Linux x86-64
- CPU with a supported public build variant
- SSD strongly recommended; HDD latency is normally unsuitable
- Enough RAM for dense/shared weights, context/KV cache, runtime overhead, and the configured expert cache

## Quick start

Portable Windows package:

1. Extract the ZIP to a writable folder.
2. Run `QwenStream.exe`.
3. Enter the path to a local `.gguf`, keep host access local, and start the server.
4. Open the chat UI from the dashboard.

CLI:

```powershell
llama-cli.exe -m C:\models\qwen.gguf --moe-paging explicit --moe-cache-mib 1536 --moe-io-threads 3 -p "Hello"
```

Server:

```powershell
llama-server.exe -m C:\models\qwen.gguf --host 127.0.0.1 --port 8080 --parallel 1 --moe-paging explicit --moe-cache-mib 1536 --moe-io-threads 3
```

OpenAI-compatible request:

```powershell
$body = @{ model = "local"; messages = @(@{ role = "user"; content = "Hello" }) } | ConvertTo-Json -Depth 4
Invoke-RestMethod http://127.0.0.1:8080/v1/chat/completions -Method Post -ContentType application/json -Body $body
```

Paging modes:

- `off`: normal resident loading.
- `os`: normal `mmap` behavior and operating-system paging.
- `explicit`: QwenStream expert slot caches and file reads.
- `auto`: chooses explicit when a cache/slot limit is supplied, otherwise OS paging. The launcher also derives a conservative cache recommendation from RAM and CPU count.

Run `qwenstream-iobench.exe --format json` to measure the buffered file path. OS caching can affect the result.

## Documentation

- [Hebrew README](README_HE.md)
- [User guide](docs/USER_GUIDE.md)
- [MoE paging](docs/MOE_PAGING.md)
- [Architecture](docs/ARCHITECTURE.md)
- [API](docs/API.md)
- [Windows build](docs/BUILD_WINDOWS.md)
- [Linux build](docs/BUILD_LINUX.md)
- [Benchmarking](docs/BENCHMARKING.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)

## Privacy and security

QwenStream has no telemetry, analytics, crash upload, or automatic model download. The launcher binds to `127.0.0.1`. If you expose `llama-server` beyond localhost, configure its API key and network controls. See [SECURITY.md](SECURITY.md).

## License and independence

The software remains MIT licensed. Model weights are not included. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [model license guidance](docs/MODEL_LICENSES.md).

QwenStream is an independent community project and is not affiliated with or endorsed by Qwen, Alibaba Cloud, ggml-org, or the Colibri authors.
