# Project map

QwenStream is a focused fork rather than a wrapper around a separately installed llama.cpp. Most directories are retained from upstream so model conversion, GGUF compatibility, server behavior, tests, and future rebases remain practical.

## QwenStream-specific areas

| Path | Purpose |
| --- | --- |
| `src/moe-paging/` | Exact expert indexing, asynchronous file reads, per-layer LRU caches, slot remapping, and statistics. |
| `tools/qwenstream/` | Local control-center executable and its browser dashboard. |
| `tools/qwenstream-iobench/` | Buffered SSD-path benchmark used for diagnostics, not as a model performance claim. |
| `config/presets/` | Conservative example configurations for small-RAM, balanced, and OS-paging setups. |
| `scripts/build-*.{ps1,sh}` | Reproducible Windows and Linux build entry points. |
| `scripts/package-windows.ps1` | Portable Windows archive staging and checksums. |
| `scripts/validate-real-model.ps1` | Explicit real-model validation entry point. |
| `packaging/` | Windows installer and portable-package support files. |
| `artifacts/*.json` | Small, tracked validation evidence. Generated release archives stay ignored. |
| `docs/` | User, build, API, architecture, model, benchmarking, and release documentation. |

## Upstream areas kept intentionally

| Path | Purpose |
| --- | --- |
| `ggml/` | Tensor library and CPU/GPU backends. |
| `src/` | llama model loading, graph construction, contexts, caches, sampling, and architecture implementations. |
| `common/` | Shared command-line parsing, chat templates, downloads, logging, and speculative decoding. |
| `tools/server/` | OpenAI-compatible HTTP server and QwenStream management endpoints. |
| `tools/ui/` | Embedded Svelte chat interface, including the QwenStream Server settings panel. |
| `conversion/` and `gguf-py/` | Hugging Face to GGUF conversion and GGUF metadata helpers. |
| `tests/` | Upstream regression suite plus `test-moe-paging`. |
| `examples/`, `grammars/`, `models/` | Upstream examples, grammar fixtures, and small tokenizer fixtures. |
| `cmake/`, `CMakeLists.txt`, `.github/` | Cross-platform build and CI infrastructure. |

## Runtime flow

1. `QwenStream` listens on `127.0.0.1:18080` and serves the local control center.
2. The manager validates startup configuration and launches the sibling `llama-server` without a command shell.
3. `llama-server` loads fixed weights normally. In explicit mode, standard routed expert weight tensors are indexed from their GGUF shards without allocating full resident copies.
4. The model router selects expert IDs. The graph resolves those IDs to bounded cache slots, reads misses exactly, and uses the existing GGML expert matmul path.
5. The chat UI talks directly to `llama-server`. Its Server settings panel talks only to the loopback manager and uses an origin allowlist.
6. MTP creates a draft context against the same compatible model. In explicit mode that context owns a separate bounded paging manager, preventing draft and target slot lifetimes from colliding.

## Generated and local-only files

The following do not belong in Git:

- `build*/`, `.build/`, `out/`, `release/`, and compiled binaries.
- Local `.gguf` models and split shards.
- `tools/ui/node_modules/`, Svelte build output, and test results.
- `*.log`, `*.tmp`, local environment files, tokens, and API keys.
- `artifacts/release/` packages and installers.

Use `git status --short` before publishing. Use `git check-ignore -v <path>` when unsure why a local file is excluded.

## Upstream synchronization

`UPSTREAM_BASE.txt` records the llama.cpp base commit. The `upstream` remote should point to `ggml-org/llama.cpp`. Keep QwenStream changes localized, update through `scripts/update-upstream.ps1` or `scripts/update-upstream.sh`, and rerun paging plus real-model validation after every rebase.
