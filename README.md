# SparkMoE

**Run giant local models on modest hardware.**

SparkMoE is an independent, CPU-first fork of `llama.cpp` that can keep standard routed MoE expert tensors on a GGUF file and load only the experts selected by the router into bounded per-layer RAM caches.

This is **v0.4.0-alpha**, refreshed onto llama.cpp stable v0.3.0. The Windows regression suite includes synthetic full-logit equivalence, real two-shard GGUF, HTTP/Desktop controls and exact Ling-3.0-tiny Q4_K_M logits with matching CPU kernels and microbatch. Qwen3.6 and Gemma 4 have separate smoke-test evidence; see [supported models](docs/SUPPORTED_MODELS.md). This remains an alpha rather than a performance or memory-fit guarantee.

## What it does

- Leaves non-routed weights on the normal `llama.cpp` path.
- Indexes routed expert tensor offsets across single or split GGUF files.
- Loads selected experts exactly, without zero weights, expert substitution, or approximation.
- Keeps bounded per-layer LRU by default, with optional hybrid global budgeting and runtime resize.
- Offers validated Windows/Linux direct I/O with buffered fallback, shared demand-priority workers, per-expert compute/I/O overlap and temporal prefetch.
- Exposes real cache/I/O statistics through C, `llama-server`, and `llama-bench`.
- Supports built-in MTP draft contexts together with bounded expert paging.
- Uses hardware-aware safe defaults and an automatic loading mode that falls back to standard mmap for dense or incompatible models.
- Keeps model selection, loading mode, expert paging, RAM status, and diagnostics in the control center; context size, batching, KV precision, context shift, and MTP live in the chat settings page.
- Includes a local-only English/Hebrew control center, a folder-backed GGUF model picker, real system RAM usage, live server state, log tail, and SSD I/O benchmark.

It does not promise that every model or quantization fits in 16 GB. Advanced hybrid/overlap/prefetch modes require CPU-only inference. Buffered per-layer paging remains the default; direct I/O and speculative prefetch are not always faster. Explicit mode validates routed expert tensor names, shapes, strides, shard bounds, and active-expert capacity before inference. See [engine measurements](docs/ENGINE_UPGRADES.md) and [refresh report](docs/REFRESH_REPORT.md).

## Hardware

- Windows 10/11 x64 or Linux x86-64
- CPU with a supported public build variant
- SSD strongly recommended; HDD latency is normally unsuitable
- Enough RAM for dense/shared weights, context/KV cache, runtime overhead, and the configured expert cache

## Quick start

Permanent Windows installation:

1. Use `sparkmoe-0.4.0-alpha-windows-x64-avx2-setup.exe` from your local `artifacts/release` build or a published release.
2. Run the installer. It installs for the current user, creates an uninstall entry, and can create desktop and Start menu shortcuts.
3. Run `SparkMoE` from the Start menu.

Portable Windows package:

1. Extract the ZIP to a writable folder.
2. Run `SparkMoE.exe`.
3. Choose a models folder, then select a `.gguf` from the discovered model list.
4. Keep **Automatic** loading unless you are testing a specific mode. It uses expert streaming for a compatible routed MoE model and standard mmap for a dense or incompatible model.
5. Start the model and open the chat UI. Context length, full-context behavior, and MTP are available under **Settings -> Context & MTP** and perform a controlled model restart.

Dense and other non-MoE GGUF models use the normal llama.cpp path and are fully supported by the launcher. Advanced loading settings expose CPU inference threads, expert slots, and an automatic/on/off CPU repack control without requiring ordinary users to change them.

Build from source on Windows:

```powershell
git clone https://github.com/menachem-dadon/SparkMoE.git
cd SparkMoE
pwsh scripts/build-windows.ps1 -Variant avx2 -RunTests
.\build-windows-avx2\bin\SparkMoE.exe
```

Use `-Variant generic` for older x64 CPUs without AVX2. Linux users can run `./scripts/build-linux.sh avx2` and then `./build-linux-avx2/bin/SparkMoE`. See the platform build guides for dependencies and troubleshooting.

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
- `explicit`: SparkMoE expert slot caches and file reads.
- `auto`: advanced CLI mode that chooses explicit when a cache/slot limit is supplied, otherwise OS paging. The dashboard defaults to `os`, which works with every supported model.

Run `sparkmoe-iobench.exe --format json` to measure the buffered file path. OS caching can affect the result.

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

SparkMoE has no telemetry, analytics, crash upload, or automatic model download. The launcher binds to `127.0.0.1`. If you expose `llama-server` beyond localhost, configure its API key and network controls. See [SECURITY.md](SECURITY.md).

## Upstream and licensing

This repository contains a pinned llama.cpp snapshot plus SparkMoE changes. It does not automatically contain upstream features added after the commit recorded in `UPSTREAM_BASE.txt`. See the [project map](docs/PROJECT_MAP.md) for the exact integration and synchronization model.

Upstream llama.cpp code and changes made directly in upstream-derived files remain under MIT. Original SparkMoE components are licensed under PolyForm Noncommercial 1.0.0. See [LICENSING.md](LICENSING.md), [LICENSE-SPARKMOE-NONCOMMERCIAL.md](LICENSE-SPARKMOE-NONCOMMERCIAL.md), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and the [model license guidance](docs/MODEL_LICENSES.md).

SparkMoE is an independent community project and is not affiliated with or endorsed by Qwen, Alibaba Cloud, Google, Gemma, ggml-org, or the Colibri authors.
