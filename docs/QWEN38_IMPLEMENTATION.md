# Qwen3.8-Flash-Next in SparkMoE

Status: implemented and validated with deterministic tiny GGUF fixtures. The published full-size model has not been downloaded or benchmarked on this machine. The 4 generated tokens/s target is therefore still an acceptance gate, not a measured result.

## Supported path

Use the published Unsloth `UD-IQ4_XS` split GGUF as the first target. It is about 93.7 GB and includes a mix of IQ3_S, IQ4_NL, IQ4_XS and Q8_0 tensors. It is acceptable when `Q4_XXS` means the published small Q4-family preset. It does not satisfy a requirement that every expert tensor contain at least four bits per weight. `UD-Q4_K_XL` is the stricter Q4 alternative, but it is about 111.3 GB and moves more expert data per generated token.

The target GGUF does not contain the native MTP head. Supply a compatible Qwen3.8-Flash-Next MTP GGUF sidecar. The reviewed dzannotti Q4_K_M sidecar is about 2.62 GB, but it remains an unverified pairing until the full-model gate below passes.

Keep at least 110 GB free for the `UD-IQ4_XS` shards, the MTP sidecar and download/build margin. More space is needed for another quantization or duplicate download files.

## What the integration does

- Imports the pinned `qwen4exp` architecture and graph-split support recorded in `config/qwen38-integration.json`. This is a selective backport; it does not claim that SparkMoE is synchronized with current llama.cpp.
- Leaves all routed and shared experts intact. Routing, top-k, tensor quantization and reduction order are unchanged.
- Keeps `per_layer_token_embd.weight` out of mmap and resident model buffers. SparkMoE computes the exact 64-bit n-gram hashes, reads the required IQ4_NL or Q5_0 rows through a bounded 4 KiB page cache, dequantizes only those rows to F32 and uploads the gathered input.
- Uses a global hybrid expert budget. Only the active layer must retain its routed top-k floor; inactive layers can be reclaimed. This removes the previous requirement to retain a top-k floor in every routed layer.
- Loads the Qwen MTP block from a separate GGUF, validates architecture, hidden width, one-head layout and vocabulary, and keeps its expert cache separate from the target cache.
- Checkpoints the complete attention, recurrent, indexer, PLE token-history and MTP draft state when rollback is required. Context shifting is disabled for PLE state because an exact shift implementation is not available.
- Accepts a Unicode model path and exposes PLE/MTP budgets plus the sidecar path through the SparkMoE manager and chat settings.

The conversion script handles the target architecture only. It does not export the Qwen MTP head; use a published sidecar.

## First full-model run

The helper does not download or modify a model. Pass the first split shard, the sidecar and this build's server:

```powershell
./scripts/run-qwen38.ps1 `
  -Model 'D:\Models\Qwen3.8-Flash-Next-UD-IQ4_XS-00001-of-00003.gguf' `
  -DraftModel 'D:\Models\Qwen3.8-Flash-Next-MTP-Q4_K_M.gguf' `
  -Server 'C:\path\to\bin\llama-server.exe'
```

The conservative baseline is CPU-only, one parallel slot, a 3 GiB global expert cache, a 64 MiB PLE page cache, a 512 MiB MTP expert cache, context 4096, microbatch 2 and MTP width 1. Start with buffered I/O and no overlap or prefetch. This isolates correctness and actual memory use before tuning. The manager exposes the same PLE and MTP settings; set the sidecar path under `Settings -> Context & MTP`.

## Required full-model gates

Do not call the integration production-ready until all of these pass with the exact target and sidecar files:

1. Verify every shard is present and record its hash, free disk space and model metadata.
2. Run prompt-logit parity without MTP between ordinary mmap on a machine that can hold it and SparkMoE paging, including prompts that cross EOS and several microbatches.
3. Compare greedy generated token IDs with MTP off and on for widths 1 and 2. Cover full acceptance, partial rejection, zero acceptance, EOS and a request immediately after EOS.
4. Measure peak process working set and committed memory through prefill and at least 512 generated tokens. The process must remain within the available RAM without sustained paging-file pressure.
5. Record target generation tokens/s, draft generation count, accepted count, target verification cost, physical SSD bytes, PLE page-cache hits and expert cache misses. Use at least three rotating prompts and a warm repeated prompt.
6. Accept the performance target only when the median sustained rate is at least 4.0 generated tokens/s and output parity still passes. A server health response or tiny-fixture result is not performance evidence.

If memory fits but throughput misses the target, tune one variable at a time: I/O mode, I/O workers, inference threads, expert budget, MTP width and MTP probability threshold. Expert dropping, reduced top-k and silent requantization are outside this implementation.

## Current validation

The Windows Release build and deterministic fixtures cover IQ4_NL and Q5_0 PLE rows across page and file-tail boundaries, bounded cache eviction, 64-bit offsets, buffered and direct I/O, full-logit parity, full hybrid/recurrent/indexer state replay, native MTP forward, accepted and rejected MTP drafts, EOS recovery, Unicode paths, manager sidecar validation and diagnostic path redaction. Existing SparkMoE dense, sharded, direct-I/O, hybrid, overlap, prefetch and resident-loading HTTP cases also pass.

This development build does not include HTTPS because OpenSSL was not available. The local chat UI was rebuilt into `tools/ui/dist` for embedding. Local ZIP and installer artifacts can be produced from this worktree, but they remain unsigned and do not replace an installed SparkMoE release unless the installer is run explicitly.
