# SparkMoE: start here in a new conversation

Updated 2026-08-31. This document replaces the need to reload the long research
and implementation conversation. Verify `git status` before doing new work.

## Decisions that must persist

- SparkMoE remains the product and the only paging engine. BigMoeOnEdge is a
  source of measured ideas, not a dependency, alternate backend or copied engine.
- Default inference is lossless relative to the selected quantized weights:
  no dropped/substituted experts, changed routing or reduced top-k.
- Preserve per-layer caches, partial-layer paging, manual pinning, Hebrew Windows
  paths, full llama-server/OpenAI APIs, chat/control UI, settings, MTP and context UX.
- Do not add Qwen3.8/Flash-Next support in a performance/maintenance task.
  Generic bounded row streaming exists; no PLE graph adapter has been implemented.
- Predictive prefetch is not shipped. A transition-predictor experiment was slower;
  its archived patch is not BMoE's hidden-state gate predictor.
- No push, public release or PR has been authorized. Local merge to main was
  explicitly requested after the user successfully installed 0.4.0-alpha.

## Current foundation

llama.cpp moved from `6b4dc2116a92c5c8f2782bfe51fabe5ee66fb5ef` to stable
`c1d0e7a004015f23bc0233470b747b596f29b264` (+644 upstream commits).
Recovery branch and tag: `sparkmoe-pre-upstream-refresh`, commit `c77dd6800`.
Implementation branch: `codex/upstream-refresh-v0.3.0`; local main receives its
history without rewriting it. Product release: 0.4.1-alpha, Windows x64 AVX2.

Read only what the new task needs:

| Need | Starting document/code |
| --- | --- |
| Cumulative changes, test limits, packaging | `docs/REFRESH_REPORT.md` |
| Actual performance observations and algorithms | `docs/ENGINE_UPGRADES.md` |
| User's minute-long wait and live settings | `docs/LATENCY_INVESTIGATION.md` |
| Upstream hooks and exact patched-file inventory | `docs/UPSTREAM_INTEGRATION.md`, `docs/UPSTREAM_PATCH_INVENTORY.md` |
| Update/rollback workflow | `docs/UPDATING_LLAMA_CPP.md` |
| Algorithm work | `src/moe-paging/`, especially paging/cache/I/O/readiness/row-stream |
| CLI/loader/graph integration | `common/sparkmoe.*`, `src/moe-paging/moe-integration.*` |
| Desktop and API | `tools/sparkmoe/` |

The original BMoE research is locally archived at
`C:/codex-output/sparkmoe-bmoe-research-20260831/DECISION_REPORT.md`.
Its alternate-backend recommendation was rejected by the user. Do not restart
that architecture discussion. The mechanism findings remain useful.

## Recommended next task: request latency tuning

The user reports correct working inference but slow visible prefill. The installed
0.4.0-alpha server was left running, with no settings changed. It used direct I/O,
hybrid 5120 MiB, 3 I/O workers, auto CPU threads, context 4096, batch 1024,
effective microbatch 32, MTP 3, overlap off, prefetch off. Its cumulative metrics
showed 334 prompt tokens in 89.876 seconds and 67 generation tokens in 19.8314
seconds. These do not isolate the reported request or prove a regression.

Version 0.4.1-alpha repairs Unicode log creation/reading and enables timestamps.
Ask the user to use it at their next convenient restart; do not kill an active
installed server. Separate initial SSE, first progress update, first generated
token, target/draft prefill and decode. Do not confuse logical batch progress
with per-token execution. Follow the one-variable A/B order in
`LATENCY_INVESTIGATION.md`; do not label suggested settings as proven faster.

The local host has 16 GB RAM and was down to about 1.4 GiB free. Never start another
large model alongside the user's running Qwen server. No giant downloads are
needed. Existing models are under `C:/models`: Ling-3.0-tiny Q4_K_M (~4.8 GB),
Qwen3.6 35B-A3B IQ4_NL (~18.5 GB), and small dense fixtures/models.

## Other bounded tasks suitable for separate conversations

1. Performance: instrument request-level timings and compare buffered/direct,
   overlap, cache policies and MTP on this hardware. Keep token output/correctness
   checks separate from timing observations; use repeated paired runs.
2. Portability: execute Linux/generic CI and direct-I/O tests. They are configured
   but were not run on this Windows host. Do not count workflow files as a hosted
   CI pass. GPU paging is not newly validated.
3. Release QA: visual Hebrew/English chat and control-panel checks, installer
   upgrade/uninstall, signing, HTTPS/OpenSSL packaging. The current native build
   supports local HTTP; OpenSSL was unavailable. Do not publish automatically.
4. Future model work: a separately authorized Qwen3.8 capability/loader/graph and
   bounded PLE row integration, with explicit RAM/read budgets and equivalence
   tests. Existing generic row infrastructure is preparation, not model support.
5. Predictive prefetch research: only in an experimental branch, only after a
   representative workload and positive repeated benchmarks justify shipping.

Each task can start in a fresh chat using this file and the relevant linked
document. There is no requirement to retain the previous conversation context.

## Useful commands

```powershell
git status --short
python scripts/sparkmoe-upstream.py check
python scripts/sparkmoe-upstream.py drift --fetch
.\scripts\update-upstream.ps1
```

The updater creates an isolated candidate and backup refs, builds/tests, and
reports GREEN/YELLOW/RED. It never commits, promotes or pushes automatically.
Use its resume mode for conflicts; do not rewrite the stable branch.

```powershell
.\scripts\build-windows.ps1 -Variant avx2 -RunTests
```

This full gate uses Desktop port 18080 and refuses to modify an existing instance.
For isolated HTTP regression while the user's Desktop is open:

```powershell
python scripts/test-sparkmoe-server.py --bin-dir build-upstream-refresh/bin --log-dir build-upstream-refresh/http-smoke --skip-manager
```

Native regressions generate tiny dense/MoE/sharded GGUF files. Real-model
equivalence: `scripts/validate-real-model.ps1`; engine observations:
`scripts/benchmark-paging.py`; HTTP/MTP smoke: `scripts/smoke-real-server.py`.
Check each script's help before choosing options. Preserve matching kernels and
microbatch for strict logit comparisons: changing resident microbatch alone
produced numerical differences in Ling, unrelated to paging correctness.

Generated builds and research logs are not tracked. Release installers/ZIPs and
SHA256 manifests are in `artifacts/release/`. Worktree validation logs and saved
patches are retained under `artifacts/validation/`; source and recovery branches
remain in Git. No model files or private request contents belong in commits.
