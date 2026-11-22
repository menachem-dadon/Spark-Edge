# Spark Edge

Spark Edge for Windows is the desktop edition. Spark Edge for Android is the mobile edition of the same product. See [Windows builds and engine packages](docs/SPARK_EDGE_WINDOWS.md).

**Run giant local models on modest hardware.**

Spark Edge runs local AI models through llama.cpp, manages a GGUF library and provides a local API. Its expert streaming reads routed MoE weights from SSD into bounded caches.

The Windows release candidate integrates the official llama.cpp revision recorded in UPSTREAM_BASE.txt. It includes the Spark Edge model manager, Hebrew and English interface options, and standalone engine packages. Model coverage and verification status are listed in [supported models](docs/SUPPORTED_MODELS.md).

## What it does

- Leaves non-routed weights on the normal `llama.cpp` path.
- Indexes routed expert tensor offsets across single or split GGUF files.
- Loads selected experts exactly, without zero weights, expert substitution, or approximation.
- Keeps bounded per-layer LRU by default, with optional hybrid global budgeting and runtime resize.
- Offers validated Windows/Linux direct I/O with buffered fallback, shared demand-priority workers, per-expert compute/I/O overlap and temporal prefetch.
- Exposes real cache/I/O statistics through C, `llama-server`, and `llama-bench`.
- Supports built-in MTP draft contexts together with bounded expert paging.
- Supports a compatible separate MTP GGUF and bounded PLE page cache for Qwen3.8-Flash-Next.
- Uses hardware-aware safe defaults and an automatic loading mode that falls back to standard mmap for dense or incompatible models.
- Keeps all Spark Edge controls in a dedicated page inside the original llama.cpp Web UI: Router limits, model profiles, loading, expert paging, context, MTP, RAM, logs, and diagnostics.
- Runs the launcher and every managed model process without a console window on Windows, while preserving complete per-session logs under the user's local application data.

Buffered per-layer paging remains the default. Explicit mode validates tensor names, shapes, strides, shard bounds, and active expert capacity. GPU paged expert support needs validation for each backend and device. See the [Windows build and validation guide](docs/SPARK_EDGE_WINDOWS.md).

## Hardware

- Windows 10/11 x64 or Linux x86-64
- CPU with a supported public build variant
- SSD strongly recommended; HDD latency is normally unsuitable
- Enough RAM for dense/shared weights, context/KV cache, runtime overhead, and the configured expert cache

## Quick start

Permanent Windows installation:

1. Use `spark-edge-0.5.0-alpha-windows-x64-avx2-setup.exe` from your local `artifacts/release` build or a published release.
2. Run the installer. It installs for the current user, creates an uninstall entry, and can create desktop and Start menu shortcuts.
3. Run Spark Edge from the Start menu.
4. While Spark Edge is running, its notification-area icon provides Open, Router status, Start, Stop, and Exit actions without a command window.

Portable Windows package:

1. Extract the ZIP to a writable folder.
2. Run `SparkMoE.exe`.
3. The llama.cpp Web UI opens directly. Open **Spark Edge** in its sidebar and choose the folder that contains your GGUF models.
4. Select a model using the normal llama.cpp model picker or the Spark Edge model profile selector.
5. Keep **Automatic** loading unless you are testing a specific mode. Context length, MTP, loading, cache, logs, and diagnostics are all under **Spark Edge**.
6. Use the Spark Edge icon in the Windows notification area to reopen the UI, inspect Router state, start or stop the Router, or exit the complete application.

Dense and other non-MoE GGUF models use the normal llama.cpp path and are fully supported by the launcher. `models-max` is user configurable: use `1` for large models, a higher value for small models that fit together, or `0` for unlimited Router instances.

Build from source on Windows:

```powershell
git clone https://github.com/menachem-dadon/Spark-Edge.git
cd Spark-Edge
pwsh scripts/build-windows.ps1 -Variant avx2 -RunTests
.\build-windows-avx2\bin\SparkMoE.exe
```

Use `-Variant generic` for older x64 CPUs without AVX2. Linux users can run `./scripts/build-linux.sh avx2` and then `./build-linux-avx2/bin/SparkMoE`. See the platform build guides for dependencies and troubleshooting.

CLI:

```powershell
llama-cli.exe -m C:\models\qwen.gguf --moe-paging explicit --moe-paging-cache-mib 1536 --moe-io-threads 3 -p "Hello"
```

Server with context and MTP:

```powershell
llama-server.exe -m C:\models\qwen.gguf --host 127.0.0.1 --port 8080 --parallel 1 --ctx-size 16384 --moe-paging explicit --moe-paging-cache-mib 1536 --moe-io-threads 3 --spec-type draft-mtp --spec-draft-n-max 3
```

Qwen3.8-Flash-Next uses `--model-draft` for its separate compatible MTP sidecar. See [the Qwen3.8 implementation and validation guide](docs/QWEN38_IMPLEMENTATION.md).

OpenAI-compatible request:

```powershell
$body = @{ model = "local"; messages = @(@{ role = "user"; content = "Hello" }) } | ConvertTo-Json -Depth 4
Invoke-RestMethod http://127.0.0.1:8080/v1/chat/completions -Method Post -ContentType application/json -Body $body
```

The Router loads the requested `model` with its saved Spark Edge process profile. Generation settings remain request-specific. A SmartiAI request can disable thinking without reloading the model by sending `reasoning_effort = "none"` and `chat_template_kwargs = @{ enable_thinking = $false }`.

Paging modes:

- `off`: normal resident loading.
- `os`: normal `mmap` behavior and operating-system paging.
- `explicit`: Spark Edge expert slot caches and file reads.
- `auto`: chooses explicit when a cache/slot limit is supplied, otherwise OS paging. It is the Spark Edge profile default.

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
- [Future WebView2 desktop integration plan](docs/WEBVIEW2_DESKTOP_INTEGRATION_PLAN.md)
- [Linux build](docs/BUILD_LINUX.md)
- [Benchmarking](docs/BENCHMARKING.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)

## Privacy and security

Spark Edge has no telemetry, analytics, crash upload, or automatic model download. The launcher binds to `127.0.0.1`. If you expose `llama-server` beyond localhost, configure its API key and network controls. See [SECURITY.md](SECURITY.md).

## Upstream and licensing

This repository contains a pinned llama.cpp snapshot plus Spark Edge changes. It does not automatically contain upstream features added after the commit recorded in `UPSTREAM_BASE.txt`. See the [project map](docs/PROJECT_MAP.md) for the exact integration and synchronization model.

Upstream llama.cpp code and changes made directly in upstream-derived files remain under MIT. Original Spark Edge components are licensed under PolyForm Noncommercial 1.0.0. See [LICENSING.md](LICENSING.md), [LICENSE-SPARKMOE-NONCOMMERCIAL.md](LICENSE-SPARKMOE-NONCOMMERCIAL.md), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and the [model license guidance](docs/MODEL_LICENSES.md).

Spark Edge is an independent community project and is not affiliated with or endorsed by Qwen, Alibaba Cloud, Google, Gemma, ggml-org, or the Colibri authors.
