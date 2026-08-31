# Installed Windows runtime: latency and performance

Snapshot: 2026-08-31, SparkMoE 0.4.0-alpha, Intel i5-1235U, 16 GB RAM,
Qwen3.6-35B-A3B UD-IQ4_NL (18.54 GB GGUF). The user confirmed that the installer
works, but reported more than a minute before visible prompt progress. Inspection
was read-only: no request was submitted, settings changed, or server restarted.

## What was measured

| Setting/counter | Observed |
| --- | --- |
| Paging | Explicit (auto resolved), all layers, hybrid cache, 5120 MiB |
| Storage | Direct I/O, 3 workers |
| Overlap / prefetch | Off / off |
| Dense residency | Auto |
| Inference threads | Auto |
| Context / batch / effective microbatch | 4096 / 1024 / 32 (requested microbatch 128) |
| MTP | On, up to 3 draft tokens |
| Main expert cache hit rate | 39.7% |
| Process working set / peak | 7.12 / 8.49 GB (decimal) |
| Available system RAM at inspection | About 1.4 GiB, rising to about 4 GiB in a later sample |
| Cumulative uncached prompt tokens / prompt time | 334 / 89.876 seconds |
| Cumulative generated tokens / generation time | 67 / 19.8314 seconds, about 3.38 tok/s |
| Cumulative MTP accepted / drafted | 47 / 63, about 74.6% |
| Requests active / deferred at inspection | 0 / 0 |

These are process-lifetime counters, not a timestamped trace of the reported
request. In particular, aggregate I/O queue time and remap wait are not the
duration of a single pause. No same-settings old/new speedup was measured here.

## Why a short message can show a long zero-progress interval

1. Typed text is not the whole model input. The chat template, system instruction
   and conversation history also consume tokens. The server actually processed
   334 uncached prompt tokens across the captured runtime. We did not inspect
   private message content or establish which request contributed those tokens.
2. Server progress is reported around logical decode batches. The initial 0%
   event precedes decode; the next event is sent in `post_decode`, after target
   decode and speculative prompt processing. A batch of 1024 can hold the entire
   334-token input even though llama internally splits it into microbatches of
   32. Zero visible progress does not prove that no prompt token is being computed.
3. Prompt tokens can select many distinct experts. A 5 GiB cache holds only part
   of this model; a 39.7% hit rate leaves substantial storage work. Direct I/O
   bypasses Windows' file cache, so it is not automatically faster on repeated
   warm reads. Low free RAM also warrants checking OS paging before allocating
   more cache. Free RAM subsequently increased, so the snapshots do not prove
   persistent memory pressure or a page-fault stall during the reported request.
4. MTP adds draft processing and a wider target verification batch. Its benefit
   depends on accepted tokens versus extra compute, reads and memory. Acceptance
   alone is not speedup. Our earlier controlled short Qwen smoke measured 2.50
   tok/s without MTP and 2.35 with MTP; this is not a universal verdict on MTP.

The installed logger also failed to create its file under a Hebrew installation
path: common logging used narrow `fopen`, and Desktop used a narrow-path stream
to read the tail. Version 0.4.1-alpha uses existing `ggml_fopen` and a UTF-8
filesystem path, and enables log timestamps. This restores evidence collection;
it does not claim to fix the minute-long inference latency.

## What was adopted from BigMoeOnEdge

Implemented inside the existing SparkMoE engine: validated direct reads with
fallback, shared demand-priority parallel I/O, hybrid global/per-layer caching,
runtime resize, per-expert compute/read overlap, temporal prefetch, dense residency
controls, richer telemetry, capability evidence and stronger sharded/full-logit
regression tests. Generic bounded row gathering is implemented but is not yet
wired into a new model's embedding/PLE graph.

This is not an identical implementation of every BMoE optimization. Our transition
predictor experiment was slower and removed from production; it is different from
BMoE's hidden-state gate predictor. Android pinned-buffer mechanisms are not
Windows implementations. Expert dropping, substitution, route-ahead and reduced
top-k were deliberately not imported. Defaults remain buffered/per-layer with
overlap and prefetch off until a workload demonstrates a repeatable benefit.

BMoE supports MTP and token n-gram drafting, and documents cases where wider
verification reads lose performance. Its Android demonstrations and deterministic
CLI benchmarks must not be conflated: Android ships a 75% *threshold relative to
uniform routing weight* for dropping cold experts, not a claim to drop 75% of all
experts. One project-reported Qwen phone experiment goes from 2.549 tok/s with
dropping off to 3.938 with that threshold. That changes model computation.
See the pinned [README](https://github.com/Helldez/BigMoeOnEdge/blob/557c63dda05ffc682b848ea67ea3682e15d75d48/README.md)
and [changelog](https://github.com/Helldez/BigMoeOnEdge/blob/557c63dda05ffc682b848ea67ea3682e15d75d48/CHANGELOG.md).
The earlier local BMoE Qwen smoke was about 2.05 tok/s and about 65 seconds for
load plus prefill to first token. Different settings and uncontrolled warmth mean
neither that run nor the phone numbers constitute a fair Spark/BMoE ranking.

## Next measurement, without changing quality

Keep the current working configuration as a saved baseline. Change one variable
at a time and record time to initial SSE/progress, actual prompt token count,
first generated token, decode rate, cache/read deltas and peak memory. Use a fresh
chat with known system text and repeat both cold-session and warm-session runs.
Do not clear the global OS file cache or compete with another large model process.

- First compare batch 64 versus 1024 while keeping effective microbatch 32.
  This should give more frequent progress opportunities; it is not a promise of
  faster total prefill and can change floating-point batching behavior.
- Then compare MTP off/on with all other settings fixed, including the same cache.
- Test inference threads 4 versus auto and I/O workers 2/3/4 independently.
- Compare buffered/direct, then overlap off/on, then per-layer/hybrid. Test
  temporal only after useful prefetch counters can be demonstrated.
- If memory pressure persists, first test MTP off and a 4096 MiB cache. Do not
  enlarge cache on the basis of model size alone. With MTP on, respect the
  recurrent rollback/microbatch guard and minimum resize budget.

These are proposed A/B settings, not changes already applied to the user's app.
The outstanding question is the request-level split between queue/setup, target
prefill, draft prefill and UI reporting. Timestamped logs plus SSE capture can
answer it; the current cumulative snapshot cannot.
