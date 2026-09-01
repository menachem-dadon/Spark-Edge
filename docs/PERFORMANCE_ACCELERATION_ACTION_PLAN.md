# SparkMoE Performance Acceleration Action Plan

Date: 2026-09-01

Repository revision: `4977a2838739b0a9c5969af4f91a0d5786cb47e0`

Research cutoff: upstream `llama.cpp` commit `0eadefebd` from 2026-09-01

## 1. Objective

This plan covers end-to-end acceleration of:

1. Prompt processing, including tokenization, prefix reuse, Qwen3.8 PLE row gathering, QSA indexer work, expert paging, and MTP prompt catch-up.
2. Token generation, including expert routing, expert cache lookup, storage reads, CPU kernels, speculative drafting, target verification, rollback, and sampling.
3. The measurement and release gates needed to prove that an optimization helps the real product without silently changing model semantics.

The primary target is lossless acceleration. Model precision, routed expert IDs, expert weights, top-k, and target verification must stay unchanged unless an option is explicitly placed in the separate quality-trading lane.

This is an investigation and implementation plan, not an implementation. No model was downloaded and no live model benchmark was started during this review.

## 2. Executive recommendation

The highest-value sequence is:

1. Fix measurement and build provenance before changing algorithms. Current server MoE counters are cumulative, the existing benchmark driver covers only a narrow decode case, and the available Release binary reports commit `061e8657d` rather than the current source revision `4977a2838`.
2. Land the low-risk prompt wins first: backport the current Qwen3.8 indexer reduction, expose separate prefill and decode thread controls, sweep batch and microbatch independently, and test the upstream AVX2 IQ prompt kernel without bypassing SparkMoE expert-readiness waits.
3. Remove hot-path allocation and kernel-object churn: reuse PLE scratch buffers, expert-routing maps, I/O jobs, Windows events, and direct-I/O bounce buffers.
4. Replace global hybrid-cache scans and repeated page decommit/commit operations with constant-time bookkeeping and retained physical slots where safe.
5. Add phase-aware expert placement. Prompt processing and decode have different routing locality, arithmetic intensity, and useful I/O policy. They should not share one fixed policy by assumption.
6. Make speculation cost-aware. MTP should adapt from measured wall time and extra bytes, not acceptance rate alone. N-gram speculation must remain disabled until its cross-request state bug is fixed and validated.
7. Treat the Intel Iris Xe backend as a separate experiment after the CPU baseline. The machine has an Xe-LP iGPU and upstream has new Xe-LP OpenCL tuning, but the current build is CPU-only and SparkMoE advanced paging modes are not validated with GPU offload.

The current full-size Qwen3.8 performance gate cannot be closed on this machine yet. The target model and MTP sidecar are not present, and drive C has only about 12.36 GiB free. The project documentation expects roughly 93.7 GB for the target model and about 2.62 GB for the sidecar, plus safe build, temporary, and page-file headroom.

## 3. Evidence and scope

### 3.1 Repository-wide inventory

The review covered the complete tracked-file inventory and then inspected the execution-critical paths in depth:

- 3,603 tracked files.
- `src/moe-paging/`: expert index, cache, global budget, readiness, row streaming, I/O scheduler, platform readers, counters, and llama integration.
- `src/models/qwen4exp.cpp`: Qwen3.8 QSA, PLE, recurrent state, and MTP graph input.
- `common/speculative.cpp`: MTP and n-gram drafting, prompt catch-up, verification hidden-state handling, and rollback interaction.
- `tools/server/`: request scheduling, prompt cache, context checkpoints, per-slot timings, cumulative metrics, and SparkMoE endpoints.
- `tools/sparkmoe/`: manager defaults, automatic recommendation logic, and server command construction.
- `tools/llama-bench/`, `scripts/benchmark-paging.py`, benchmark configurations, raw local JSON results, and performance documentation.
- Build configuration, current binaries, integration manifests, upstream delta, model inventory, and current machine state.

This was a repository-wide structural scan plus a line-level hot-path review. It was not a claim that every vendored or unrelated backend source line is a SparkMoE performance bottleneck.

### 3.2 Current machine

| Component | Observed state |
| --- | --- |
| System | Dell Vostro 3520 |
| CPU | Intel Core i5-1235U, 10 cores, 12 logical processors, 12 MiB L3 |
| Memory | 15.69 GiB, 2 x 8 GiB DDR4, configured at 2667 MT/s |
| Storage | Solidigm P41PL 512 GB NVMe |
| Free C drive | About 12.36 GiB at review time |
| GPU | Intel Iris Xe Graphics, Xe-LP family |
| Current build | Release, AVX2 and OpenMP on; AVX-VNNI, AVX-512, native tuning, BLAS, OpenCL, Vulkan, and SYCL off |
| Available large MoE | Qwen3.6-35B-A3B IQ4_NL, 17.26 GiB |
| Qwen3.8 full target | Not present |

This host is a useful low-memory CPU/NVMe target. Its hybrid P/E core layout and limited DRAM bandwidth make thread count and affinity especially important. Results from a server CPU, discrete GPU, or phone must not be copied as local expectations.

### 3.3 Current evidence boundary

- Qwen3.8 PLE and MTP have deterministic tiny-fixture coverage.
- Full-size Qwen3.8 memory use, output parity, sustained speed, and the 4 generated tokens/s gate remain unmeasured.
- Qwen3.6 has short real-model observations, not a completed long-run validation matrix.
- Ling measurements show that direct I/O, hybrid caching, overlap, and prefetch are not universal wins.
- The installed or available runtime metrics can describe cumulative server work. They do not prove the latency source for one request without start/end deltas.

## 4. What the current measurements already say

### 4.1 I/O mode

The local paired Ling observations in `build-upstream-refresh/e1-benchmark.json` show:

| Mode | Observed generated tok/s samples |
| --- | --- |
| Buffered, 1 worker | 6.74, 7.08 |
| Buffered, 4 workers | 10.15, 10.75 |
| Direct, 4 workers | 4.10, 4.17 |

This is strong evidence that direct I/O is not the default choice for this workload and storage state. It is not proof that buffered always wins: direct I/O may help when the model is far beyond RAM and the Windows page cache creates destructive double caching. The engine needs an attributable cold/warm matrix and physical-byte accounting, not a hard-coded slogan.

### 4.2 Hybrid cache

Short paired samples were approximately 5.4 to 6.0 tok/s for both per-layer and hybrid policies. The project already states that these runs do not establish a hybrid speed advantage.

The hybrid policy is still valuable as a memory-capacity mechanism, especially for Qwen3.8 with many experts. The immediate opportunity is to reduce its own bookkeeping and virtual-memory overhead before asking whether its placement policy wins.

### 4.3 Overlap and prefetch

Overlap sometimes improved short runs, but the same matrix also had large order variance. Temporal prefetch had zero recorded prefetch reads in some runs and no stable advantage at a 512 MiB cache. The transition predictor read substantially more data and was slower than the temporal control.

Action: keep overlap opt-in until request-level stall metrics prove a benefit, and keep the rejected transition predictor out of the runtime.

### 4.4 MTP

A short Qwen3.6 observation measured about 2.50 tok/s without MTP and 2.35 tok/s with MTP. Another runtime snapshot reported 47 accepted of 63 drafted tokens, yet only about 3.38 generated tok/s. Acceptance alone therefore does not establish acceleration.

Action: split MTP time into target prompt, MTP catch-up, draft, target verification, rollback, and waiting for paged experts. Optimize and enable it only on positive end-to-end evidence.

## 5. Performance model

### 5.1 Prompt processing

For one request, time to first generated token should be modeled as:

`queue + template/tokenize + prefix lookup/restore + target prefill + PLE/QSA + expert I/O/cache + MTP catch-up + final synchronization`

Prompt tokens/s alone hides queueing, prompt-cache restore cost, MTP catch-up, and the final synchronization that produces the first streamed token. Report both uncached prompt throughput and end-to-end TTFT.

### 5.2 Token generation

Steady-state latency per emitted token should be modeled as:

`routing/cache bookkeeping + expert readiness/I/O stall + expert kernels + dense/attention kernels + draft cost + target verify cost + rollback + sampling/streaming`

For speculative decoding, divide the total by accepted emitted tokens, not drafted or verified positions. A wider verification batch is a loss when additional expert reads and state handling cost more than the target passes it avoids.

### 5.3 Bottleneck classification

Every run should be classified from evidence:

| Evidence | Likely regime | First response |
| --- | --- | --- |
| High I/O wait, high bytes/token, low hit rate | Storage/cache bound | Increase useful residency, coalesce reads, overlap verified demand, reduce wasted prefetch |
| Low I/O wait, high compute time, more threads do not help | DRAM bandwidth or kernel bound | Tune physical-core count, kernel/repack path, reduce copies |
| High major faults or falling available RAM | Memory-pressure bound | Reduce graph/KV/cache budgets, change dense residency, stop page-cache duplication |
| Long prompt only | Prefill/QSA/PLE bound | Wider legal batch, prompt kernels, indexer and PLE optimization, prefix reuse |
| High draft acceptance but slower wall time | Verification or MTP-head bound | Reduce width, raise threshold, disable MTP for that workload |
| High p95 with acceptable median | Eviction/I/O tail bound | Fix queueing, coalescing, residency, thermal or memory-state variance |

## 6. Code findings and concrete actions

### 6.1 Manager configuration prevents independent prefill tuning

`tools/sparkmoe/sparkmoe.cpp` stores one `inference_threads` value and passes it to both `--threads` and `--threads-batch`. Decode is mostly narrow GEMV and memory bandwidth; prompt processing is wider GEMM and can profit from a different thread count.

Actions:

1. Replace the single manager field with `generation_threads` and `prompt_threads`, preserving backward-compatible loading of old settings.
2. Add separate draft generation and draft prompt thread controls where llama.cpp exposes them.
3. Add CPU-mask, strict-affinity, and poll controls to the experimental benchmark layer before exposing them as normal UI settings.
4. Build a host tuner that tests physical P-cores, all physical cores, and all logical processors rather than assuming more workers are faster.

Expected value: high for both prompt and generation, low implementation risk.

### 6.2 Prompt RAM cache is disabled by the manager

Upstream server defaults include prompt caching, but SparkMoE forces `--cache-ram 0`. This disables cross-slot saved prompt states and idle-slot saving. Same-slot prefix reuse can still occur, but a repeated system/tool prefix is not preserved in the RAM prompt cache.

Actions:

1. Measure real request tokenization and `n_prompt_tokens_cache` before changing the default.
2. Add a small bounded prompt-cache experiment, such as 128, 256, and 512 MiB, with expert-cache and KV budgets reduced by the exact same amount.
3. Use varied user tails behind a fixed system/tool prefix. Do not benchmark by repeating an identical full prompt.
4. Set checkpoint spacing relative to the configured context and observed message boundaries. The current 8,192-token minimum makes most checkpoints irrelevant at a 4,096-token context.
5. Fingerprint cache entries by model, quantization, chat template, tool schema, sampling-relevant state, and any adapter that affects KV state.

Expected value: potentially very high for agent and multi-turn workloads, but only when the prefix is actually reused and RAM is not taken from a more valuable expert cache.

### 6.3 Global hybrid eviction scans all layers and experts

`cache_budget::oldest()` scans every registered layer and every entry for each global eviction. It also refreshes completion state and calls `resident_count()`, which itself scans entries. Qwen3.8 has enough layers and experts for this bookkeeping to matter on frequent misses.

Actions:

1. Maintain an intrusive global evictable LRU, or one per size class, updated on hit, load completion, pin, active-reference change, and prefetch promotion.
2. Track resident counts incrementally.
3. Keep active and loading entries off the evictable list instead of filtering them during a full scan.
4. Add counters for eviction-selection CPU time, scanned entries, and retry loops.

Acceptance: identical victim ordering under a deterministic trace, identical logits, and a measurable fall in cache-management microseconds/token under miss-heavy decode.

Expected value: medium to high for hybrid mode, medium risk.

### 6.4 Hybrid eviction decommits and immediately recommits slot pages

On a miss, `resolve()` calls `evict(slot)` and then `commit_slot(slot)`. In hybrid mode, eviction discards every tensor pool range for the slot and commit restores it. On Windows this translates into expensive virtual-memory operations even when the same layer immediately reuses the slot.

Actions:

1. Split logical eviction from physical decommit.
2. Retain committed pages for same-layer slot reuse when the global budget already owns that capacity.
3. Decommit only when capacity transfers to another layer, the budget shrinks, or the system memory-pressure controller requests it.
4. Account committed bytes by physical slot ownership so the budget remains exact.

Acceptance: committed-byte counters remain correct across resize, pinning, failed reads, cancellation, and rollback; Windows virtual-memory calls per generated token fall substantially.

Expected value: high on Windows hybrid mode, medium risk.

### 6.5 Expert resolution allocates and hashes on every routed batch

`layer_cache::resolve()` creates a vector of unique experts, an `unordered_map`, a reserved bitmap, and a miss list for each routing request. It later builds shared read-batch state for overlap. Decode top-k is small and expert IDs are bounded.

Actions:

1. Allocate routing scratch once per layer or worker.
2. Replace the hash table with a dense expert-to-local-index array plus generation stamps, or a small sorted vector when top-k is small.
3. Reuse reserved and miss storage.
4. Avoid constructing a `read_batch` when there are no misses or no consumer that can overlap.
5. Record allocation count and bytes in the decode hot path.

Expected value: medium, low to medium risk.

### 6.6 I/O executor creates a packaged task and future per read

The shared executor allocates a `packaged_task`, shared ownership, and a future for each tensor-component read. One missing expert can produce separate gate, up, and down reads. Demand promotion and cancellation also search queued prefetch work.

Actions:

1. Use a bounded reusable I/O request pool with explicit completion state.
2. Replace linear queue searches with tag-indexed handles or intrusive queue nodes.
3. Batch all tensor components for one expert into one logical request and completion object.
4. Sort or group demand reads by shard and source offset when doing so does not delay a critical first projection.
5. Test two-wave loading: issue gate/up early, then down, only if the graph has a real compute window between them.
6. Export queue depth, service time, queue time, coalesced bytes, and cancellation waste per request.

Expected value: high in I/O-bound cases, medium to high risk because readiness and cancellation must stay exact.

### 6.7 Windows async reads create and destroy an event per chunk

`moe-io-windows.cpp` opens the file for overlapped I/O, but each read loop creates an event, submits one `ReadFile`, waits immediately, reads the result, and closes the event. This preserves correctness but pays kernel-object cost and does not obtain the full benefit of Windows asynchronous I/O.

Actions in increasing complexity:

1. Reuse one event and `OVERLAPPED` structure per I/O worker.
2. Keep several worker-owned operations in flight.
3. Evaluate an I/O completion port backend for batched demand reads.
4. Preserve the current simple backend as a fallback and compare end-to-end, not just raw throughput.

Expected value: medium to high for small or frequent reads, medium risk for event reuse, high risk for IOCP.

### 6.8 Direct I/O allocates a bounce buffer per read

The aligned reader allocates up to an 8 MiB aligned bounce buffer inside each `read_exact()` call. It can also read alignment padding and copy the requested bytes into the destination.

Actions:

1. Reuse a worker-local aligned buffer sized to the largest observed request class.
2. Pool a small number of buffers when a worker can have multiple reads in flight.
3. Report logical bytes, physical bytes, padding amplification, fallbacks, and copy time separately.
4. Use direct I/O only when its lower page-cache pressure outweighs padding and copy cost on the measured workload.

Expected value: high for direct mode, low to medium risk.

### 6.9 PLE row streaming reallocates scratch repeatedly

`moe-row-stream.cpp` allocates mapped/reserved/missing/future vectors per chunk. Page-mode gathering builds piece lists and page-ID lists, sorts them, allocates a page scratch buffer, and scans all pieces for each page chunk. `copy_rows_f32()` also allocates ID and raw-byte vectors on every call.

Actions:

1. Move bounded vectors and raw buffers into reusable row-stream scratch.
2. Pre-reserve for the documented 256-row maximum.
3. Build a direct page-to-piece index so each piece is visited once after page sorting.
4. Deduplicate repeated PLE rows with a dense stamp table before reading.
5. Benchmark 4, 16, and 64 KiB page sizes against prompt lengths and quant formats.
6. Reduce lock scope after correctness is proven; today a single mutex covers mapping, I/O wait, conversion, and copies.

Expected value: high for Qwen3.8 prompt processing, medium risk.

### 6.10 Qwen3.8 PLE hash input allocates inside every token loop

`llm_graph_input_ple::set_input()` creates the row-index vector per microbatch and a new `std::vector<int64_t>` for every token. It recomputes each n-gram XOR from the beginning. After row streaming, it dequantizes into a float staging vector and copies it again into the backend input tensor.

Actions:

1. Reuse the row-index vector as class scratch.
2. Use fixed-size stack or class scratch for the token window.
3. Build the n-gram mixed hash incrementally as the window grows instead of repeating prior products/XORs.
4. Reuse or directly target the final CPU input buffer when backend ownership allows it.
5. Investigate a CPU gather/dequant graph op, then a fused gather/dequant/first-projection path. Keep this behind parity and backend fallbacks.

Expected value: medium for short prompts and high for large prompt batches where PLE streaming is active; low risk for scratch reuse, high risk for fusion.

### 6.11 Qwen3.8 QSA is missing a current upstream prompt optimization

Upstream commit [`09412af38`](https://github.com/ggml-org/llama.cpp/commit/09412af38a9fe328da2ed452a40f310eea350290) removes a redundant query copy and replaces transpose plus `sum_rows` with a short sum of indexer-head slices. Upstream reported 2,170 to 2,366 prompt tokens/s on an RTX PRO 6000 at 55k context, with generation unaffected and greedy tokens unchanged.

The local source still uses `ggml_cont(q)`, a full permute/copy, and `ggml_sum_rows` for the head reduction.

Actions:

1. Backport this one-file change through the existing pinned-integration process.
2. Re-run Qwen3.8 tiny full-logit tests.
3. Measure CPU prompt sizes 512, 2,048, and 8,192. Do not assume the GPU percentage transfers to this CPU.

Expected value: high confidence, prompt-only, low risk.

### 6.12 Upstream AVX2 IQ batched prompt kernel is relevant but needs paging integration

Upstream commit [`85c55223c`](https://github.com/ggml-org/llama.cpp/commit/85c55223c) adds a batched AVX2 GEMM path for grid IQ quantizations and a `MUL_MAT_ID` fast path. This is relevant to the local Qwen3.6 IQ4_NL model and potentially to Qwen3.8 IQ builds.

Risk: SparkMoE currently waits for an expert at the normal CPU `MUL_MAT_ID` path. A new dispatch path must not consume a paged expert before its read completes, and CPU repack must not assume a stable resident source buffer if the slot is remapped.

Actions:

1. Isolate the upstream commit and enumerate every `MUL_MAT_ID` dispatch it can bypass.
2. Put the readiness wait at a shared dispatch boundary or add an equivalent wait to the IQP path.
3. Test resident, per-layer, direct, hybrid, overlap, eviction, and failed/cancelled read cases.
4. Benchmark prompt batches large enough to activate the IQP threshold.

Expected value: potentially high for prompt processing, high integration risk.

### 6.13 MTP repeats prompt work in a separate context

For non-shared memory models, `common_speculative_impl_draft_mtp::process()` copies target hidden rows and decodes every target prompt batch through the MTP context. It also resizes and copies per-sequence verification hidden-state buffers. This is necessary for the current design but directly increases TTFT.

Actions:

1. Add timers for target prompt, MTP catch-up, hidden-state copies, draft head, target verification, rollback, and paging wait.
2. Reserve maximum legal hidden-state buffers once per slot and reuse them.
3. Avoid copying rows that cannot become the next draft seed.
4. Investigate deferred or pipelined MTP catch-up only after profiling proves the catch-up dominates TTFT. This must preserve recurrent state and prompt checkpoints.
5. Allow MTP to be enabled for generation while explicitly reporting its added TTFT.

Expected value: high for MTP-enabled prompt latency, medium risk for buffer reuse, high risk for pipelining.

### 6.14 MTP policy uses acceptance, but should use saved wall time

One verification batch can activate up to draft-width times as many distinct experts. High acceptance can still lose when the extra expert reads, MTP head, hidden-state copies, or rollback are expensive.

Actions:

1. Track per-window `draft_us`, `verify_us`, `accepted_tokens`, target passes avoided, MTP bytes, verify bytes, and rollback bytes/time.
2. Estimate saved time as `plain_target_cost * accepted_tokens - measured_speculation_cost`.
3. Adapt width among 0, 1, 2, and 3 using a conservative exponentially weighted score with hysteresis.
4. Reduce width immediately after repeated negative windows or memory pressure.
5. Keep a per-model, per-quant, per-context profile; do not carry one workload's policy into another without a cold validation window.

Expected value: high and protects against regressions, medium risk.

### 6.15 N-gram cache has an unresolved cross-request correctness/performance bug

Local `common_speculative_impl_ngram_cache::begin()` is a no-op. Upstream issue [`#27852`](https://github.com/ggml-org/llama.cpp/issues/27852) demonstrates that a slot can retain the prior request's context n-grams, collapse acceptance, and become slower than no speculation.

There is a separate upstream issue, [`#26100`](https://github.com/ggml-org/llama.cpp/issues/26100), showing that a draft-cache replay path can bypass `p_min` and inflate identical-prompt benchmarks.

Actions:

1. Keep `ngram-cache` disabled in product profiles.
2. Fix or adopt upstream fixes for per-request reset and replay confidence handling.
3. Add same-slot long-then-short and mixed-prefix request tests in existing speculative test infrastructure.
4. Benchmark varied prompts and held-out repeated structures, never only the exact warm-up prompt.
5. Prefer zero-cost decline behavior: if no strong match exists, run an ordinary one-token target decode.

Expected value: useful for copy-heavy or structured output after fixes; currently a blocker, not an optimization.

### 6.16 CPU kernels should be phase-aware

KTransformers demonstrates the general principle that high-arithmetic-intensity prompt batches and low-intensity decode need different kernel choices and task granularity. This host and current build do not provide a supported AMX or AVX-512 path, so its exact kernels are not portable from KTransformers. The principle is portable:

1. Use GEMM-oriented packing and scheduling for prompt batches.
2. Use low-overhead GEMV paths for decode.
3. Fuse gate/up work or expert task scheduling only when the active quant and llama.cpp operator layout make it maintainable.
4. Avoid re-reading or repacking an expert on each remap. A prepacked sidecar or slot-local packed cache is an optional later design, fingerprinted to CPU ISA, quant layout, model hash, and build ABI.

Expected value: potentially very high, high implementation and maintenance cost. Do this after measurement, upstream prompt wins, and hot-path allocation cleanup.

### 6.17 Intel Iris Xe is an experiment, not a current recommendation

The host has Intel Iris Xe. Upstream commit [`010be9683`](https://github.com/ggml-org/llama.cpp/commit/010be9683) tunes Xe-LP OpenCL quantized prompt and generation kernels. The current build has OpenCL off, and hybrid/overlap/prefetch currently force CPU-only operation.

Experiment sequence:

1. Build current upstream CPU and OpenCL baselines with a resident small model.
2. Verify Qwen3.8 operator coverage and output correctness.
3. Measure dense/attention offload while experts remain CPU-resident or paged.
4. Account unified-memory pressure and host/device copies.
5. Only then design mixed paging support. Do not weaken the CPU-only guard before the readiness and memory contracts are proven.

Expected value: unknown; medium research value, high product-integration risk.

## 7. Lessons from related systems

| Source | Reusable lesson | What not to copy blindly |
| --- | --- | --- |
| [Colibri](https://github.com/JustVugg/colibri) | Treat storage, RAM, and VRAM as tiers; learn routing heat; use pinned hot experts plus adaptive LRU; keep route telemetry; preserve model semantics by default | Hot-expert learning can overfit and some balanced routers are nearly flat; O_DIRECT and lookahead are host dependent |
| [BigMoeOnEdge](https://github.com/Helldez/BigMoeOnEdge) | Per-token I/O/cache/compute telemetry, conditional cooling between runs, 256-token steady windows, overlap A/B, pressure and fault metrics | Phone defaults and direct-I/O results do not transfer to this Windows laptop; its lossy expert dropping is not a lossless SparkMoE default |
| [MoE-Infinity](https://github.com/EfficientMoE/MoE-Infinity) | Request-level activation traces, activation-aware caching, prefetch, dedicated I/O workers, continuous batching | It targets GPU/host/SSD serving and a different runtime; reported paper factors are not local predictions |
| [KTransformers](https://github.com/kvcache-ai/ktransformers) | Phase-specific kernels, cache-aware weight layout, fused expert tasks, dynamic load balancing | The host and current build do not provide its AMX/AVX-512 path; its BF16/INT8 layouts also differ from this fork |
| [llama.cpp speculative docs](https://github.com/ggml-org/llama.cpp/blob/master/docs/speculative.md) | Record implementation-level draft statistics and use SPEED-Bench for varied end-to-end comparisons | Acceptance rate is not throughput, and repeated-prompt cache paths can invalidate a benchmark |

The common design rule is to expose a planner and evidence, not to silently activate every technique. An optimization should move from hypothesis to default only after a controlled end-to-end win on the supported machine/model class.

## 8. Prioritized optimization register

### 8.1 P0: measurement and provenance

| ID | Action | Impact | Risk | Exit gate |
| --- | --- | --- | --- | --- |
| M1 | Attach MoE/PLE snapshot deltas to each request | Enables all later decisions | Low | One request reports only its own hits, misses, bytes, waits, and evictions |
| M2 | Split request time into queue, tokenize/template, prefix restore, target prompt, MTP catch-up, generation, and stream completion | High diagnostic value | Low | Timings sum to wall time within documented async tolerance |
| M3 | Extend benchmark driver to PP and TG, cold/warm, repeated-prefix and varied prompts | Prevents false wins | Low | Reproducible manifest and raw output for every cell |
| M4 | Embed source SHA, integration base, compiler, CMake flags, CPU features, model hash, and command in every result | Prevents stale-binary claims | Low | A result is rejected if provenance is incomplete |
| M5 | Record temperature, frequency, available RAM, major faults, process RSS/private bytes, and SSD bytes | Explains variance | Low | Every performance table includes entry state and pressure |

### 8.2 P1: low-risk prompt wins

| ID | Action | Expected effect | Risk |
| --- | --- | --- | --- |
| PP1 | Backport Qwen3.8 indexer slice reduction `09412af38` | Lower long-prompt QSA work | Low |
| PP2 | Separate `threads` and `threads-batch` in manager and tuner | Better PP/TG balance | Low |
| PP3 | Reuse PLE index, token-window, page, raw, and float scratch | Lower allocation and copy overhead | Low |
| PP4 | Sweep legal `batch` and `ubatch` independently by model and memory budget | Better prompt GEMM utilization | Low |
| PP5 | Build clean current-HEAD portable and native/ISA variants | Establish real baseline | Low |
| PP6 | Bounded prompt RAM cache experiment for repeated system/tool prefixes | Can eliminate repeated prefill | Medium memory trade |

### 8.3 P2: expert cache and I/O hot path

| ID | Action | Expected effect | Risk |
| --- | --- | --- | --- |
| IO1 | Reusable Windows events and direct-I/O bounce buffers | Lower per-read overhead | Low to medium |
| IO2 | Reusable I/O request/completion pool | Lower allocation/synchronization cost | Medium |
| IO3 | Global constant-time evictable LRU and incremental counts | Lower hybrid miss bookkeeping | Medium |
| IO4 | Retain same-layer committed pages on logical eviction | Avoid VirtualAlloc/VirtualFree churn | Medium |
| IO5 | Batch/coalesce component reads by expert/shard/offset | Fewer I/O operations | Medium to high |
| IO6 | Adaptive worker count and buffered/direct selection from measured queue and pressure | Avoid bad fixed defaults | Medium |
| IO7 | Phase-specific cache/pinning based on held-out route traces | Higher decode hit rate | Medium to high |

### 8.4 P3: kernels and graph path

| ID | Action | Expected effect | Risk |
| --- | --- | --- | --- |
| K1 | Integrate upstream AVX2 IQP prompt kernels with paging readiness | High PP gain for IQ quants | High |
| K2 | Direct PLE gather/dequant into CPU graph input | Remove a staging copy | Medium to high |
| K3 | Fuse PLE gather/dequant with first projections where maintainable | Lower memory traffic | High |
| K4 | Evaluate slot-local or sidecar prepacked expert weights | Faster expert compute | High storage/ABI cost |
| K5 | Fuse/schedule expert gate/up/down tasks by phase | Lower scheduling and tail imbalance | High |

### 8.5 P4: generation and speculation

| ID | Action | Expected effect | Risk |
| --- | --- | --- | --- |
| TG1 | Tune generation threads and P-core affinity separately | Lower per-token latency | Low |
| TG2 | Enable overlap only from positive request-level stall evidence | Hide useful I/O | Low policy risk |
| TG3 | Reuse MTP hidden-state and draft scratch | Lower MTP overhead | Low to medium |
| TG4 | Adapt MTP width from saved wall time and bytes | Avoid negative speculation | Medium |
| TG5 | Fix n-gram cross-request/replay bugs and test varied traffic | Safe copy-heavy acceleration | Medium |
| TG6 | Use learned pins only after held-out trace validation | Better cache locality | Medium |
| TG7 | Try CPU+iGPU dense/attention offload after CPU baseline | Possible compute relief | High |

### 8.6 Separate quality-trading lane

The following can be faster but are not lossless and must never be merged into the default quality policy:

- Fewer active experts than the model requests.
- Dropping low-weight cache-miss experts.
- Cache-aware expert substitution or rerouting.
- Route-ahead that changes expert IDs.
- More aggressive weight or KV quantization.
- Shorter context or disabled reasoning that changes the task.

Each needs a named UI/CLI warning, deterministic configuration when possible, speed metrics, output diffs, and task-quality evaluation. A small qualitative sample cannot establish no quality loss.

## 9. Benchmark protocol

### 9.1 Required models

1. Tiny deterministic fixtures for exact logits, state replay, paging, PLE, and MTP mechanics.
2. Ling-3.0-tiny for resident versus paged full-logit comparison.
3. Local Qwen3.6-35B-A3B IQ4_NL for a real 16 GiB-class MoE baseline.
4. Full Qwen3.8-Flash-Next target plus the exact compatible MTP sidecar for the release gate.

The Qwen3.8 gate needs additional storage or cleanup first. Leave enough free space for the model set, temporary files, build artifacts, logs, and Windows page-file operation; do not begin the download with 12.36 GiB free.

### 9.2 Workload cells

Prompt processing:

- 64, 512, 2,048, and 8,192 input tokens where context permits.
- A fixed system/tool prefix with rotating user tails.
- Entirely distinct prompts of comparable token length.
- First request after process start, warm model with cold request prefix, and repeated-prefix cache hit.

Generation:

- At least 256 generated tokens for tuning; 512 for the Qwen3.8 acceptance gate.
- At least three rotating prompt families: prose, code/structured text, and multilingual/chat.
- Plain target, MTP widths 1/2/3, and n-gram only after correctness blockers close.
- Greedy decoding for parity and an application-realistic sampling cell for product behavior.

### 9.3 Experimental discipline

1. Change one independent variable per primary A/B.
2. Record the complete command and configuration.
3. Warm up separately, then use a steady window. Do not count the cold cache head as steady decode.
4. Run forward and reverse order. Randomize longer matrices.
5. Use at least five valid repetitions per cell; add more when confidence intervals overlap.
6. Gate the next cell on measured temperature, CPU frequency, and available RAM rather than a fixed sleep.
7. Reject contaminated cells with thermal throttling, memory collapse, large major-fault changes, background load, or build mismatch.
8. Report median, p5/p95, and a bootstrap confidence interval, not only a mean.
9. Keep raw per-token or per-request records next to the summarized result.

### 9.4 Metrics

Prompt metrics:

- Queue delay.
- Template and tokenization time.
- Input tokens, cached tokens, and actually processed tokens.
- Prompt decode time and tokens/s.
- TTFT from request arrival and from decode start.
- PLE rows requested, unique rows, hits, rows read, logical/physical bytes, conversion and copy time.
- QSA/indexer operator time when profiling is enabled.
- MTP catch-up time and bytes.

Generation metrics:

- Emitted tokens/s and milliseconds per emitted token.
- Per-token p50/p95 latency.
- Expert hits, misses, duplicate routes, evictions, logical and physical bytes/token.
- I/O queue, service, remap wait, and compute wait time.
- Overlap events and compute-with-I/O-pending boundaries.
- Prefetch issued, useful, promoted, wasted, and bytes wasted.
- Drafted and accepted tokens by position.
- Draft, verify, rollback, and target time per emitted token.
- CPU cycles/instructions/cache misses where a repeatable profiler is available.
- Process peak RSS/private bytes, system available-memory floor, major faults, SSD throughput/latency, CPU temperature and frequency.

### 9.5 Correctness gates

Lossless paging and I/O changes:

- Full logits or the strongest existing deterministic oracle against resident execution.
- Greedy token IDs and EOS behavior.
- Split-shard boundaries, direct fallback, failed/cancelled reads, eviction, pinning, and resize.
- Multi-turn prompt cache and context shift.

Prompt-kernel changes:

- Backend operator tests in existing files.
- Qwen3.8 tiny full-logit parity.
- Real-model greedy comparison using the same batch/kernel conditions where bit identity is expected.

MTP and n-gram:

- Accepted tokens must match the target verification result.
- Correct recurrent state after fully accepted, partially accepted, rejected, EOS, and context-shift paths.
- Same-slot mixed requests and prompt-cache restore.
- Do not require byte-identical text to an unbatched run when batch-dependent floating-point order changes; instead use target-verification correctness plus explicit output comparison and quality gates.

### 9.6 Performance acceptance

An optimization is a default candidate only when:

1. The lower confidence bound is positive on the intended workload and the median improvement is operationally meaningful, normally at least 3 to 5 percent for a hot-path change.
2. p95 token latency does not regress by more than 5 percent without an explicit tail-latency trade.
3. Peak private bytes, available-memory floor, and major faults stay within the declared budget.
4. It wins on at least two prompt families or is deliberately scoped to a named workload.
5. All relevant correctness gates pass.

The Qwen3.8 project gate remains: median sustained rate of at least 4.0 emitted tokens/s over at least 512 generated tokens, three rotating prompts, acceptable memory pressure, and output/state parity gates.

## 10. Host-specific tuning matrix

This matrix is for measurement, not a claimed best configuration.

### 10.1 CPU and build

| Variable | Candidate cells |
| --- | --- |
| Generation threads | 2, 4, 6, 10 |
| Prompt threads | 4, 6, 10, 12 |
| Affinity | Windows default, P-cores only, all physical cores, all logical processors |
| Build | Current portable AVX2, `GGML_NATIVE=ON`, verified AVX-VNNI build if CPU support is confirmed |
| BLAS | Off versus one controlled BLAS build for long prompt only; prevent OpenMP oversubscription |
| CPU repack | Off versus on, with paging compatibility and memory measured separately |

### 10.2 Prompt batching

| Variable | Candidate cells |
| --- | --- |
| Logical batch | 64, 256, 1,024, 2,048 if memory permits |
| Microbatch | Model-legal 2/6 baseline, then 16, 32, 64, 128 as memory permits |
| Context | 4,096 baseline; larger only for actual workload need |
| Prompt RAM cache | 0, 128, 256, 512 MiB with equal total-memory budget |
| Checkpoint spacing | Off, message-boundary-aware, 1,024/2,048/8,192 token spacing as context permits |

For recurrent/MTP models, enforce the existing minimum microbatch contract relative to draft width. A high microbatch that causes memory pressure or reduces expert-cache capacity can lose despite faster kernels.

### 10.3 Expert paging

| Variable | Candidate cells |
| --- | --- |
| Expert cache | 1,536, 3,072, 4,096 MiB; 5,120 only with a safe available-memory floor |
| Cache policy | Per-layer LRU, hybrid after hot-path cleanup |
| I/O workers | 1, 2, 4; test 8 only if queue and storage evidence justify it |
| I/O mode | Buffered 2/4 versus direct 2/4, cold and warm |
| Overlap | Off/on after per-request wait accounting |
| Prefetch | Off; temporal only after useful-read counters are non-zero |
| Dense residency | mmap versus resident/anonymous-equivalent with major faults recorded |

### 10.4 Speculation

| Variable | Candidate cells |
| --- | --- |
| MTP | Off, width 1, 2, 3 |
| Confidence | 0, 0.4, 0.6, 0.8 where supported |
| Draft threads | 1, 2, 4 and a separate prompt-thread sweep if exposed |
| Policy | Fixed width versus wall-time adaptive |
| N-gram | Disabled until bugs close; then copy-heavy and ordinary prompts |

### 10.5 iGPU

Only after a clean CPU baseline:

- Current-source OpenCL build with upstream Xe-LP tuning.
- Resident small-model PP/TG operator coverage.
- Dense/attention offload with CPU experts.
- Unified-memory pressure, copy time, and correctness.

## 11. Delivery roadmap

### Phase 0: evidence foundation

Effort: approximately 2 to 4 focused engineering days.

Deliverables:

- Clean Release build at current source SHA.
- Request-level MoE/PLE deltas and timing spans.
- Extended benchmark manifest and raw-result schema.
- Machine-state capture and result validator.
- Baseline report for local Qwen3.6 and tiny fixtures.

Exit: every claimed delay can be assigned to prompt, target generation, MTP, compute, cache, or I/O for one request.

### Phase 1: safe configuration and prompt wins

Effort: approximately 3 to 7 days.

Deliverables:

- Separate prompt/generation threads.
- Qwen3.8 indexer slice reduction.
- PLE scratch reuse and rolling hash cleanup.
- CPU/build/batch/ubatch sweep.
- Bounded prompt-prefix cache experiment.

Exit: a documented local safe profile with confidence intervals and no parity regression.

### Phase 2: cache and I/O hot-path rewrite

Effort: approximately 1 to 3 weeks, split into reviewable increments.

Deliverables:

- Reusable Windows I/O objects and bounce buffers.
- Reusable routing/I/O request storage.
- Constant-time hybrid eviction structure.
- Logical eviction separated from physical decommit.
- Read grouping/coalescing experiment.

Exit: lower cache-management and I/O overhead on miss-heavy real-model decode, with exact paging gates intact.

### Phase 3: prompt kernels and phase-aware placement

Effort: approximately 1 to 3 weeks.

Deliverables:

- IQP integration feasibility and readiness-safe prototype.
- Direct PLE input path.
- Prompt versus decode routing trace analysis.
- Held-out learned pinning and phase-specific policy experiment.

Exit: real prompt and decode wins that survive held-out prompts and reverse-order runs.

### Phase 4: speculation policy

Effort: approximately 1 to 2 weeks after Phase 0 telemetry.

Deliverables:

- MTP phase counters and reusable buffers.
- Wall-time/byte-aware adaptive width.
- N-gram state fixes and mixed-traffic regression coverage.
- SPEED-Bench-compatible varied-prompt campaign.

Exit: speculation auto-disables when negative and produces a positive end-to-end result on its supported workload.

### Phase 5: optional backend and deep-kernel research

Effort: open-ended; require a design review before implementation.

Candidates:

- Intel Xe-LP OpenCL mixed execution.
- Prepacked expert sidecar/cache.
- Fused expert tasks and PLE first projection.
- IOCP backend.
- Hardware-aware automatic resource planner.

These are large changes. They should start only after the contributor can explain the layout, readiness, state, and fallback contracts and maintain the change independently.

## 12. Recommended implementation order by file

1. `tools/server/server-common.h`, `tools/server/server-context.cpp`, `tools/sparkmoe/server-api.cpp`: per-request timing and MoE snapshot deltas.
2. `scripts/benchmark-paging.py` and existing benchmark configuration files: PP/TG workload schema, provenance, machine state, and varied prompts.
3. `tools/sparkmoe/sparkmoe.cpp` plus its existing tests: separate prompt/generation/draft thread controls.
4. `src/models/qwen4exp.cpp`: upstream indexer reduction, then PLE scratch and hash cleanup.
5. `src/moe-paging/moe-row-stream.cpp`: reusable PLE page/raw scratch and one-pass piece mapping.
6. `src/moe-paging/moe-io-aligned.cpp`, `moe-io-windows.cpp`, `moe-io-common.cpp`: buffer/event/request reuse.
7. `src/moe-paging/moe-cache.cpp` and headers: constant-time eviction and retained commitment.
8. `common/speculative.cpp`: MTP timing/buffer reuse/adaptive policy and n-gram reset after upstream review.
9. `ggml/src/ggml-cpu/`: IQP readiness integration only after the prior gates and design review.

Do not add a new test file without maintainer approval. Extend the closest existing deterministic and server test infrastructure.

## 13. Stop rules

Stop or revert an experiment when any of the following occurs:

- Expert IDs, top-k, weights, or quantization change in the lossless lane.
- A read can become visible before its completion state is published.
- Failed or cancelled I/O leaves a slot mapped to incomplete data.
- Recurrent/MTP rollback differs after rejection or context shift.
- A speed result depends on identical prompt replay, stale slot state, or cumulative counters.
- Median improves but p95, major faults, or available-memory floor crosses the declared limit.
- Direct I/O reads more physical bytes and does not reduce page pressure enough to repay the cost.
- Prefetch useful reads remain zero or wasted bytes exceed saved demand bytes.
- Learned pinning loses on held-out prompts.
- A proposed change requires a broad upstream refresh that overwrites SparkMoE Qwen3.8 paging/state work instead of a reviewed three-way integration.

## 14. Documentation and maintenance debt

Before declaring a performance release:

1. Update `docs/MOE_PAGING.md`, which still says direct I/O is rejected and does not describe shipped hybrid/overlap/prefetch behavior.
2. Update or archive `docs/NEXT_SESSION.md`, which still says no Qwen3.8 PLE graph adapter exists.
3. Make `docs/ENGINE_UPGRADES.md` explicit about which measurements are old Ling snapshots and which are current Qwen evidence.
4. Generate one canonical capabilities/defaults page from the actual argument schema or tests to reduce drift.
5. Keep correctness, memory-fit, package, and speed evidence in separate status fields.

## 15. Immediate next actions

1. Free or add enough storage for a safe full Qwen3.8 validation environment; do not download into the current 12.36 GiB free space.
2. Produce a clean current-HEAD Release binary and embed its SHA and CMake flags in results.
3. Add request-level MoE/PLE start/end snapshots and the missing MTP timing spans.
4. Expand the benchmark driver to prompt lengths, 256/512-token generation, rotating prompts, and machine-state gates.
5. Establish the Qwen3.6 CPU baseline with buffered I/O, MTP off, and no experimental prefetch.
6. Sweep generation and prompt threads independently, including P-core affinity.
7. Backport and validate Qwen3.8 indexer commit `09412af38`.
8. Reuse PLE and direct-I/O scratch buffers.
9. Implement constant-time hybrid eviction bookkeeping and same-layer commitment retention as separate changes.
10. Evaluate MTP only from end-to-end saved time; keep n-gram disabled until cross-request bugs are closed.
11. Review IQP readiness integration before importing `85c55223c`.
12. Run the full Qwen3.8 memory/parity/512-token/three-prompt gate, then decide whether the 4 tok/s goal is met or which measured subsystem remains dominant.

## 16. Primary references

- [SparkMoE Qwen3.8 implementation guide](QWEN38_IMPLEMENTATION.md)
- [SparkMoE engine measurements](ENGINE_UPGRADES.md)
- [SparkMoE benchmarking guide](BENCHMARKING.md)
- [Colibri repository and measured design principles](https://github.com/JustVugg/colibri)
- [Colibri routing telemetry](https://github.com/JustVugg/colibri/blob/main/docs/routing-telemetry.md)
- [BigMoeOnEdge repository](https://github.com/Helldez/BigMoeOnEdge)
- [BigMoeOnEdge benchmark method](https://github.com/Helldez/BigMoeOnEdge/blob/main/docs/benchmark-method.md)
- [BigMoeOnEdge MTP analysis](https://github.com/Helldez/BigMoeOnEdge/blob/main/docs/mtp.md)
- [MoE-Infinity repository](https://github.com/EfficientMoE/MoE-Infinity)
- [MoE-Infinity paper](https://arxiv.org/abs/2401.14361)
- [KTransformers AMX and phase-specific kernel design](https://github.com/kvcache-ai/ktransformers/blob/main/doc/en/AMX.md)
- [llama.cpp speculative decoding documentation](https://github.com/ggml-org/llama.cpp/blob/master/docs/speculative.md)
- [llama.cpp Qwen3.8 indexer prompt optimization](https://github.com/ggml-org/llama.cpp/commit/09412af38a9fe328da2ed452a40f310eea350290)
- [llama.cpp AVX2 IQ batched prompt kernel](https://github.com/ggml-org/llama.cpp/commit/85c55223c)
- [llama.cpp Intel Xe-LP OpenCL tuning](https://github.com/ggml-org/llama.cpp/commit/010be9683)
- [llama.cpp n-gram cross-request bug](https://github.com/ggml-org/llama.cpp/issues/27852)
- [llama.cpp draft replay benchmark bug](https://github.com/ggml-org/llama.cpp/issues/26100)
