# GiantLLM

**Run giant local models on modest hardware.**

GiantLLM is an independent, CPU-first fork of `llama.cpp` that can keep standard routed MoE expert tensors on a GGUF file and load only the experts selected by the router into bounded per-layer RAM caches.

This is **v0.2.0-alpha**. Qwen3.5/Qwen3.6 MoE paging has completed deterministic tests and a short real-model streaming regression. Gemma 4 26B-A4B and other models using standard three-dimensional routed expert tensors are now accepted through layout-based validation. The combined Gemma/MTP/explicit-paging real-model matrix is not complete, so this remains an alpha rather than a performance or memory-fit guarantee.

## What it does

- Leaves non-routed weights on the normal `llama.cpp` path.
- Indexes routed expert tensor offsets across single or split GGUF files.
- Loads selected experts exactly, without zero weights, expert substitution, or approximation.
- Uses bounded per-layer LRU slot pools and asynchronous buffered file I/O.
- Exposes real cache/I/O statistics through C, `llama-server`, and `llama-bench`.
- Supports built-in MTP draft contexts together with bounded expert paging.
- Controls context size, batching, KV precision, context shift, MTP, and paging from both the control center and the chat settings page.
- Includes a local-only English/Hebrew control center, live server state, log tail, hardware recommendations, and SSD I/O benchmark.

It does not page GPU weights, support direct/unbuffered expert I/O, or promise that every model or quantization fits in 16 GB. Explicit mode validates routed expert tensor names, shapes, strides, shard bounds, and active-expert capacity before inference.

## Hardware

- Windows 10/11 x64 or Linux x86-64
- CPU with a supported public build variant
- SSD strongly recommended; HDD latency is normally unsuitable
- Enough RAM for dense/shared weights, context/KV cache, runtime overhead, and the configured expert cache

## Quick start

Permanent Windows installation:

1. Download `giantllm-0.2.0-alpha-windows-x64-avx2-setup.exe` from the release page.
2. Run the installer. It installs for the current user, creates an uninstall entry, and can create desktop and Start menu shortcuts.
3. Run `GiantLLM` from the Start menu.

Portable Windows package:

1. Extract the ZIP to a writable folder.
2. Run `GiantLLM.exe`.
3. Enter the path to a local `.gguf` and apply the conservative hardware recommendation.
4. Set context and MTP options if the model supports them, then start the server.
5. Open the chat UI. Later changes are available under **Settings -> Server** and perform a controlled model restart.

Build from source on Windows:

```powershell
git clone https://github.com/menachem-dadon/GiantLLM.git
cd GiantLLM
pwsh scripts/build-windows.ps1 -Variant avx2 -RunTests
.\build-windows-avx2\bin\GiantLLM.exe
```

Use `-Variant generic` for older x64 CPUs without AVX2. Linux users can run `./scripts/build-linux.sh avx2` and then `./build-linux-avx2/bin/GiantLLM`. See the platform build guides for dependencies and troubleshooting.

CLI:

```powershell
llama-cli.exe -m C:\models\qwen.gguf --moe-paging explicit --moe-cache-mib 1536 --moe-io-threads 3 -p "Hello"
```

Server with context and built-in MTP:

```powershell
llama-server.exe -m C:\models\qwen.gguf --host 127.0.0.1 --port 8080 --parallel 1 --ctx-size 16384 --moe-paging explicit --moe-cache-mib 1536 --moe-io-threads 3 --spec-type draft-mtp --spec-draft-n-max 3
```

OpenAI-compatible request:

```powershell
$body = @{ model = "local"; messages = @(@{ role = "user"; content = "Hello" }) } | ConvertTo-Json -Depth 4
Invoke-RestMethod http://127.0.0.1:8080/v1/chat/completions -Method Post -ContentType application/json -Body $body
```

Paging modes:

- `off`: normal resident loading.
- `os`: normal `mmap` behavior and operating-system paging.
- `explicit`: GiantLLM expert slot caches and file reads.
- `auto`: chooses explicit when a cache/slot limit is supplied, otherwise OS paging. The launcher also derives a conservative cache recommendation from RAM and CPU count.

Run `giantllm-iobench.exe --format json` to measure the buffered file path. OS caching can affect the result.

## Documentation

- [Hebrew README](README_HE.md)
- [User guide](docs/USER_GUIDE.md)
- [MoE paging](docs/MOE_PAGING.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Project map](docs/PROJECT_MAP.md)
- [Licensing map](LICENSING.md)
- [Supported models](docs/SUPPORTED_MODELS.md)
- [API](docs/API.md)
- [Windows build](docs/BUILD_WINDOWS.md)
- [Linux build](docs/BUILD_LINUX.md)
- [Benchmarking](docs/BENCHMARKING.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)

## Privacy and security

GiantLLM has no telemetry, analytics, crash upload, or automatic model download. The launcher binds to `127.0.0.1`. If you expose `llama-server` beyond localhost, configure its API key and network controls. See [SECURITY.md](SECURITY.md).

## Upstream and licensing

This repository contains a pinned llama.cpp snapshot plus GiantLLM changes. It does not automatically contain upstream features added after the commit recorded in `UPSTREAM_BASE.txt`. See the [project map](docs/PROJECT_MAP.md) for the exact integration and synchronization model.

Upstream llama.cpp code and changes made directly in upstream-derived files remain under MIT. Original GiantLLM components are licensed under PolyForm Noncommercial 1.0.0. See [LICENSING.md](LICENSING.md), [LICENSE-GIANTLLM-NONCOMMERCIAL.md](LICENSE-GIANTLLM-NONCOMMERCIAL.md), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and the [model license guidance](docs/MODEL_LICENSES.md).

GiantLLM is an independent community project and is not affiliated with or endorsed by Qwen, Alibaba Cloud, Google, Gemma, ggml-org, or the Colibri authors.
