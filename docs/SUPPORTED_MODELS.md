# Supported models

QwenStream separates base llama.cpp model support from explicit SSD expert paging support.

## Support levels

| Model family | Base inference | Explicit expert paging | Built-in MTP | Current validation |
| --- | --- | --- | --- | --- |
| Qwen3.5/Qwen3.6 MoE | Upstream implementation | Supported | Supported when the GGUF contains MTP tensors | Deterministic paging suite plus a short Qwen3.6 35B-A3B streaming regression. Full parity matrix pending. |
| Gemma 4 26B-A4B | Upstream implementation | Supported through standard routed tensor layout detection | Supported when a compatible MTP head is present | Build and structural tests. Resident-versus-paged real-model matrix pending. |
| Other MoE models with standard `blk.N.ffn_*_exps.weight` tensors | Upstream-dependent | Accepted after strict load-time layout validation | Upstream-dependent | Experimental until a named model is added to validation evidence. |
| Dense models | Upstream implementation | Not applicable | Upstream-dependent | Normal llama.cpp behavior. |

## What standard routed layout means

Explicit paging recognizes only routed expert weights ending in `.weight` for the gate, up, down, or fused gate-up roles. The expert axis must be the third tensor dimension with a valid nonzero stride. Tensor names must identify a nonnegative `blk.N` layer. Scale, bias, shared-expert, attention, embedding, output, and unknown tensors stay on the normal loader path.

If a model does not satisfy these checks, loading fails explicitly. QwenStream does not substitute experts, invent offsets, or continue with zero weights.

## MTP notes

MTP is optional and can reduce performance on some CPUs, backends, quantizations, or prompts. Start with three draft tokens and compare measured end-to-end throughput against MTP off. A GGUF without compatible MTP tensors will fail to initialize the draft context; disable MTP for that model.

## Adding a validated family

1. Confirm upstream llama.cpp loads and runs the resident model correctly.
2. Inspect the GGUF expert tensor roles, shapes, quantization, and shard layout.
3. Run deterministic paging tests.
4. Compare resident and paged output or logits with fixed prompts and sampling.
5. Record raw configuration, build commit, hardware, model hash, counters, and failures under `artifacts/`.
6. Update this matrix only after the evidence is reviewable.
