# SparkMoE engine checkpoints

One SparkMoE engine, no BigMoeOnEdge dependency or copied implementation. Default
routing, top-k and expert weights are unchanged. The existing research report is
reference evidence; architecture-specific Qwen3.8 work remains out of scope.

## E1: direct I/O and shared scheduling

`--moe-io-mode direct` now opens a private Windows `FILE_FLAG_NO_BUFFERING` handle
or Linux `O_DIRECT` file description. It does not change the upstream loader's
file flags. Sector/page alignment, bounded bounce buffers, a byte-comparison
probe and file-range validation protect unaligned GGUF offsets. Unsupported
filesystems, failed direct reads and unaligned shard tails use the independent
buffered reader. Windows reopening preserves native Unicode paths.

All layers of one context share `--moe-io-threads` workers. Demand jobs precede
queued prefetch jobs; running OS reads are not preempted. The speculative queue
is bounded. This is the scheduling foundation, not yet a prefetch implementation.
Telemetry adds physical bytes, direct bytes and fallback reads to the existing
logical-byte counters. Rebuild consumers together with the refreshed llama DLL;
this fork does not promise binary ABI compatibility across upstream refreshes.

Windows Release build and native unit/full-logit tests passed. Tests cover native
direct access, Unicode, unaligned offsets, multi-chunk reads, EOF, forced failure,
fallback and demand priority. The generated GGUF model also passes direct-mode
parity across two shards. HTTP off/buffered, explicit/buffered and explicit/direct
all passed completions, chat, SSE, config, telemetry and reset.

Existing local Ling-3.0-tiny Q4_K_M: 1,886,208 full-logit values match exactly
between resident, buffered paging and direct paging when both sides use the
same non-repacked CPU kernels. Default resident CPU repacking uses a different
numeric path and did not meet the strict 1e-4 elementwise tolerance; that is not
claimed as bitwise equivalence. No tolerance was weakened.

Paired forward/reverse tg8 x 2 observations, 4 CPU compute threads, 1,500 MiB cache:

| I/O configuration | Measured tok/s samples | Peak process RSS |
| --- | --- | --- |
| Buffered, 1 worker | 6.74, 9.60, 7.08, 11.44 | about 2.26 GB |
| Buffered, 4 workers | 10.15, 12.85, 10.75, 13.53 | about 2.26 GB |
| Direct, 4 workers | 4.10, 6.56, 4.17, 6.68 | about 2.26 GB |

These short local observations include warm OS-cache effects, not a cold-SSD
benchmark or proof of broad performance gains. Buffered remains the default.
Linux code is present but not yet locally compiled; WSL lacks the build toolchain.

Reproduce using an existing model (no download):

```powershell
build-upstream-refresh/bin/test-moe-regression.exe --model C:/models/model.gguf
python scripts/benchmark-paging.py --bin build-upstream-refresh/bin/llama-bench.exe --model C:/models/model.gguf --output artifacts/bench/paging.json
```

## E2: hybrid cache and runtime resize

`--moe-cache-policy lru` keeps the existing fixed per-layer pools. The opt-in
`hybrid` policy keeps the same layer-cache code, reserves virtual address space,
and commits only page-aligned expert slices. A global LRU can transfer budget
between layers while protecting a top-k floor, pinned experts and the current
graph's active experts. Idle pages are decommitted on Windows and discarded on
Linux. Reserved virtual address space is not reported as committed cache RAM.

`POST /sparkmoe/v1/paging/cache` with `{"cache_mib": 1536}` queues a hybrid budget
change at the next synchronized decode boundary. HTTP 202 acknowledges the
request, not its eventual success. Monitor `resize_pending_bytes`,
`resize_failures`, `cache_limit_bytes` and `cache_committed_bytes`. Shrinks below
per-layer floors or protected/pinned residency are rejected without changing the
old limit. Growing the budget does not automatically enlarge the context's
microbatch; shrinking safely clamps it. Target and MTP contexts own separate
caches/budgets. The resize API targets the main context only.

Hybrid mode is CPU-only. It changes residency and offsets, never expert routing,
top-k, quantization or weight bytes. Page padding is internal cache layout, not a
GGUF format change. `moe-capabilities.*` records model-specific evidence separately
from generic layout acceptance and upstream model support.

Windows Release build, native unit/synthetic full-logit tests and four HTTP
cases passed. The two-shard hybrid fixture exercises actual eviction and resize;
unit checks cover global borrowing, layer floors, active leases and pinned shrink
rejection. Ling's 1,886,208 logits remain exactly equal with matching CPU kernels
across resident, fixed, direct and hybrid (including runtime shrink/grow).
The teacher-forced trace committed 926,720,000 expert bytes in hybrid versus
1,552,637,952 in fixed pools; a cache need not evict when that trace fits.

Paired tg8 x 2 observations at 1,500 MiB, four CPU and I/O threads:

| Policy | tok/s samples | Peak RSS |
| --- | --- | --- |
| Per-layer | 5.64, 5.87, 5.53, 6.16 | about 2.31 GB |
| Hybrid | 6.00, 4.97, 5.43, 4.81 | about 2.39 GB |

These samples do not establish a hybrid speed advantage. Virtual addressing,
page padding and a shared LRU also have costs. Per-layer remains the default.
Logs: `build-upstream-refresh/e2-real-parity.log`, `e2-benchmark.json` and
`server-smoke-e2/`. Build/HTTP activity was finished before the paired benchmark.

## Remaining checkpoints

- E3: per-expert readiness, compute/I/O overlap, temporal prefetch and measured
  predictive-prefetch evaluation. New modes remain opt-in.
- E4: dense/shared residency controls, richer memory/stall/cache telemetry and
  generic bounded row streaming, without a Flash-Next-specific graph adapter.
- Release: repeat gates, complete updater candidate validation, package all
  runtime dependencies, build installer/ZIP and validate the staged product.
