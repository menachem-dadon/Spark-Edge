# Supported models

SparkMoE separates base llama.cpp model support from explicit SSD expert paging support.

## Support levels

| Model family | Base inference | Explicit expert paging | Built-in MTP | Current validation |
| --- | --- | --- | --- | --- |
| Qwen3.8-Flash-Next (`qwen4exp`) | Selective pinned upstream backport | Supported with bounded PLE page gathering and global hybrid expert budget | Supported through a compatible separate one-head MTP GGUF | Tiny IQ4_NL/Q5_0 full-logit parity, direct/buffered PLE, full state replay, accepted/rejected drafts, EOS recovery and manager launch pass on Windows CPU. Published full-size Q4 memory, output and 4 tok/s gates are pending. |
| Qwen3.5/Qwen3.6 MoE | Upstream implementation | Supported | Supported when the GGUF contains MTP tensors | Refreshed Windows Qwen3.6 35B-A3B IQ4_NL HTTP generation and MTP passed, identical 12-token greedy output; 7/12 draft tokens accepted. Full-logit/quantization matrix pending. |
| Ling-3.0-tiny / bailingmoe3 | Upstream implementation | Validated Q4_K_M on Windows CPU | Not tested | Exact 1,886,208-value full-logit parity with matching kernels and microbatch: resident/fixed/direct/hybrid/overlap/temporal, eviction/resize and dense residency policies. |
| Gemma 4 26B-A4B | Upstream implementation | Supported through standard routed tensor layout detection | Supported when a compatible MTP head is present | Gemma 4 26B-A4B Q4_0 load, prompt, generation, and process-survival regression with explicit paging, CPU repack, and multiple threads. Resident-versus-paged logit parity pending. |
| Other MoE models with standard `blk.N.ffn_*_exps.weight` tensors | Upstream-dependent | Accepted after strict load-time layout validation | Upstream-dependent | Experimental until a named model is added to validation evidence. |
| Dense models | Upstream implementation | Not applicable | Upstream-dependent | Normal llama.cpp behavior, including verified automatic fallback from expert streaming to standard mmap. |

## What standard routed layout means

Explicit paging recognizes only routed expert weights ending in `.weight` for the gate, up, down, or fused gate-up roles. The expert axis must be the third tensor dimension with a valid nonzero stride. Tensor names must identify a nonnegative `blk.N` layer. Scale, bias, shared-expert, attention, embedding, output, and unknown tensors stay on the normal loader path.

If a model does not satisfy these checks, loading fails explicitly. SparkMoE does not substitute experts, invent offsets, or continue with zero weights.

## MTP notes

MTP is optional and can reduce performance on some CPUs, backends, quantizations, or prompts. Start with three draft tokens and compare measured end-to-end throughput against MTP off. Recurrent rollback requires effective microbatch at least draft tokens + 2, with enough expert slots/cache. With a smaller expert paging microbatch, the server uses sequence checkpoints. Runtime resize preserves the minimum when bounded recurrent rollback is active. A GGUF without compatible MTP tensors will fail to initialize the draft context; disable MTP for that model.

Hybrid cache, compute/I/O overlap and temporal prefetch are opt-in CPU modes.
Direct-I/O fallback is byte-exact. Default routing/top-k remain unchanged.
Ling comparisons deliberately fix microbatch and CPU repacking: different
upstream numeric paths do not establish bitwise equivalence across batching.
The evidence registry is `src/moe-paging/moe-capabilities.cpp`. Linux and GPU
validation must not be inferred from Windows CPU results. Gemma evidence above
predates this refresh and was not repeated in the final matrix.

Qwen3.8-Flash-Next has a model-specific PLE adapter and MTP sidecar path. See
`docs/QWEN38_IMPLEMENTATION.md` for the exact boundary and pending full-model gates.

## Adding a validated family

1. Confirm upstream llama.cpp loads and runs the resident model correctly.
2. Inspect the GGUF expert tensor roles, shapes, quantization, and shard layout.
3. Run deterministic paging tests.
4. Compare resident and paged output or logits with fixed prompts and sampling.
5. Record raw configuration, build commit, hardware, model hash, counters, and failures under `artifacts/`.
6. Update this matrix only after the evidence is reviewable.
