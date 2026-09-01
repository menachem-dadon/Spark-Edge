# SparkMoE refresh and engine upgrade report

Implementation branch: `codex/upstream-refresh-v0.3.0`. The user subsequently
authorized local promotion to `main` after successfully installing 0.4.0-alpha.
No push, upstream PR or history rewrite is part of this work. Final maintenance
entry point for a fresh conversation: `docs/NEXT_SESSION.md`.

## Recovery and upstream

- Original SparkMoE: `c77dd680079bd98bbfe3eef8c65c49503956d002`.
- Recovery branch and tag: `sparkmoe-pre-upstream-refresh`.
- Previous llama.cpp: `6b4dc2116a92c5c8f2782bfe51fabe5ee66fb5ef`.
- Refreshed llama.cpp: `c1d0e7a004015f23bc0233470b747b596f29b264`,
  official stable v0.3.0, 644 upstream commits later.
- `upstream/master` at selection was 90 commits beyond that stable release.
  We deliberately selected the recent stable release, not an untested moving tip.
- The existing local SparkMoE `v0.3.0` tag collides with the upstream tag name;
  the updater fetches into its private ref and never overwrites release tags.

## Completed foundation

The merge preserves both histories. Conflicts were resolved for the new upstream
load-mode APIs, common_json, graph/model loading, server lifecycle and routes,
CMake, and reorganized chat UI settings/PWA files. Additional Windows fixes cover
Unicode resource paths and Visual Studio environment initialization, embedded
Web UI byte arrays, private unit-test linkage, and benchmark default params.

SparkMoE remains one paging engine and product. BigMoeOnEdge was used as research
and mechanism reference, with no source import, backend process or new dependency.
Existing settings, MTP/context controls, OpenAI server, chat UI, Hebrew paths,
partial-layer paging, per-layer caches and manual pin/unpin remain in scope.

Integration implementations moved into `common/sparkmoe.*`,
`src/moe-paging/moe-integration.*`, and `tools/sparkmoe/server-api.*`. The common
tensor hook replaces the model-specific Qwen3.5 loader patch. Core algorithm code
stays under `src/moe-paging/`; extracted MIT integration retains its MIT license.
The upstream-derived src/common/include/server patch dropped from 525 inserted
lines/21 deletions to 203/16 after extraction (19 files), then 244/16 after adding
the engine API and memory telemetry. The final Unicode logging correction makes
that 245/17 across 20 files. The CPU readiness
extension adds 17 lines in three GGML files. The whole maintained upstream-file
inventory, including branding/build/docs/UI, contains 47 files in 10 groups.
See `UPSTREAM_PATCH_INVENTORY.md` for every file and line count and
`config/upstream-integration.json` for checked markers.

## Maintenance

```powershell
.\scripts\update-upstream.ps1
```

The updater checks a clean tree, fetches the stable target, reports the delta,
creates backup refs and an isolated worktree, enables rerere, attempts a merge,
checks integration markers, builds and runs native/HTTP regressions. It never
promotes or commits a candidate automatically. GREEN means all gates passed;
YELLOW means review is needed even though gates passed; RED identifies a failed
stage and retains the candidate and logs. The stable checkout is not modified.
`docs/UPDATING_LLAMA_CPP.md` documents conflicts, resume and rollback.

Fast Windows/Linux CI uses generated tiny GGUF models, including real two-shard
files, and does not download model weights. Hosted CI results must be reported
separately from local Windows validation.

## Engine and validation ledger

See `docs/ENGINE_UPGRADES.md` for measured mechanisms and limitations. E1 was
validated with Windows direct I/O, alignment/EOF/fallback and priority tests,
synthetic full-logit and HTTP tests, and 1,886,208 real Ling Q4_K_M logit values
matching exactly with identical CPU kernels. Buffered I/O remains the default:
direct I/O was slower in the measured warm-cache workload.

### New engine capabilities

- E2: optional hybrid global LRU budget over the existing per-layer cache, page
  commitment/decommit, layer floors, active leases, pin protection and synchronized
  runtime resize. A named model/capability evidence registry and stronger shard
  layout validation separate tested families from merely accepted layouts.
- E3: per-expert/projection readiness permits CPU compute before all reads finish.
  Demand-first scheduling, prefetch promotion/cancellation/draining, retry on
  speculative failure and bounded temporal prefetch use that same engine.
- E4: generic bounded row gather, dense/shared mmap/resident/locking controls,
  exposed partial-layer selection and Desktop controls/telemetry. The row service
  addresses huge logical tables through fixed row budgets without materializing
  them. It is not a PLE model adapter.
- Predictive transition prefetch was implemented experimentally, passed synthetic
  correctness and was measured slower on the paired Ling workload. It is excluded
  from production; an archived patch and reproducible benchmark configuration
  preserve the experiment. No quality-changing optimization was introduced.
- Defaults remain buffered, per-layer, overlap off and prefetch off. Direct I/O,
  hybrid and overlap did not demonstrate a universal speed win. Measurements,
  RAM observations and exact reproduction commands are in ENGINE_UPGRADES.md.

### Validation evidence

- Windows MSVC Release CPU native unit and synthetic full-logit suites pass.
  Coverage includes activation/off/auto/OS, partial layers, scales/original expert
  IDs, pins, eviction, independent target/draft caches, resize, async readiness,
  cancellation, I/O errors and unload. Tiny real two-shard GGUF fixtures require
  no downloads. Synthetic model outputs are correctness fixtures, not language
  quality samples.
- Six HTTP configurations pass completions, OpenAI chat, SSE completion, config,
  telemetry and reset, including a Hebrew model path. Desktop start/generation,
  resize, restart, stop, config and redacted diagnostics pass without a browser.
- Seven real-Git updater safety tests pass: dirty tree, conflict, uncommitted
  merge isolation, failed gate/base retention, missing hook, YELLOW after success
  and unregistered upstream patch rejection before build.
- Ling Q4_K_M: every mode matches 1,886,208 full-logit values exactly at 512 MiB,
  exercising hundreds of evictions and runtime shrink/grow. Dense mmap versus
  resident also matches exactly. Repacking and microbatch are fixed on both
  sides. Upstream's resident-only change from microbatch 8 to 1 has max delta
  1.4616346; cross-batch numerical identity is not claimed or hidden by a relaxed
  tolerance.
- Qwen3.6 35B-A3B IQ4_NL: refreshed HTTP generation and MTP pass. Both produce the
  same 12-token greedy text; 7 of 12 MTP draft tokens accepted. This is not a full
  model/quantization logit matrix. The MTP cache/microbatch assertion discovered
  during validation is fixed by an explicit load-time guard and resize minimum.
- Chat UI Svelte check/build passed earlier in the refresh. Dashboard JS syntax
  passed after new controls. Full visual/browser UX regression was not performed.
- Linux/generic/GPU and hosted GitHub Actions were not executed locally. Linux
  source and CI are included, but the local WSL lacks a C++/CMake toolchain.
  Previous Gemma smoke evidence was not repeated on this refreshed build.

## Packaging and final validation

Product versions: 0.4.0-alpha plus the 0.4.1-alpha Unicode logging correction
(independent of llama.cpp's stable version).
Local hardware: Intel Core i5-1235U, 12 logical CPUs, 16 GB RAM; measured model
runs used four inference threads unless their individual test says otherwise.
The Windows packaging script now includes llama/GGML/server DLLs and app-local
MSVC runtime DLLs; the old EXE-only staging would not make this shared build
portable. It also stages upstream and chat-UI dependency licenses and a per-binary SHA256
manifest with source and binary build revisions. MSVC OpenMP's VCOMP140.DLL is
included in addition to the CRT. The 0.4.0-alpha installer and ZIP were built;
an extracted ZIP passed all 30 binary hashes, restricted-PATH CLI/server/bench
startup, the I/O tool, dense/MoE/sharded HTTP tests and Desktop lifecycle/resize/
diagnostics. The user then installed that version and reported that it works
excellently, at least as well as the previous version. This user feedback is
separate from our automated tests; no visual browser QA was claimed.

The actual end-to-end updater created an isolated worktree and backup refs,
resolved the latest stable target to c1d0e7a, built all native targets and the
custom UI, and passed native, six HTTP configurations, Desktop and seven updater
safety tests. The result was YELLOW, not RED: npm deprecations, UI chunk size and
build-environment warnings remain. The scoped UI dependency update was followed
by a second passing full gate. OpenSSL was unavailable, so this Windows package
supports local HTTP but does not claim HTTPS support. Linux/generic/GPU and
hosted CI remain unexecuted here. Packages are unsigned and were not published.

Read-only investigation of the installed runtime found 334 cumulative uncached
prompt tokens taking 89.876 seconds and generation at about 3.38 tok/s. The
current settings use direct I/O and hybrid 5 GiB with MTP, but overlap/prefetch
off; free host RAM was about 1.4 GiB. Batch-level progress can remain zero while
prefill is already running. No request-level trace existed because logging used
an ANSI file open under the Hebrew installation path. Version 0.4.1-alpha fixes
that log writer/reader and enables timestamps. It does not claim a proven speedup
or that the reported minute-long latency is resolved. See
`LATENCY_INVESTIGATION.md` for evidence, BMoE comparisons and proposed A/B settings.
The installed app and its configuration were left untouched.

The logging correction passed both native suites and six isolated HTTP cases,
including real native log creation at a Hebrew path. The seven updater safety
tests also passed again. Desktop lifecycle is intentionally skipped on this
last pass while the user's installed manager occupies port 18080; its previous
0.4.0-alpha package test remains the lifecycle evidence.

The final all-target Windows build also passed after the logging correction.
Validated Mermaid/nanoid UI assets and dependencies were consolidated back into
the original checkout. Both temporary updater worktrees were removed only after
archiving their patches, modified files, state, configuration and validation logs
under `artifacts/validation/worktree-checkpoints-20260831/`. Recovery refs and the
implementation branch are retained. Earlier build folders and release binaries
were not deleted. Build/research artifacts remain outside tracked source.

The 0.4.1-alpha installer and ZIP were then built successfully. Extracted-ZIP
validation passed all 30 binary hashes, required runtime DLLs, CLI/server/bench
startup with a restricted PATH, the I/O tool, and dense/MoE/two-shard HTTP cases
including direct + hybrid + overlap + temporal. The native build revision is
`061e8657d`; subsequent handoff documentation does not change native code. Final
artifacts are `artifacts/release/sparkmoe-0.4.1-alpha-windows-x64-avx2-setup.exe`
and the corresponding `.zip`, with `SHA256SUMS` and a per-binary manifest.

Local promotion uses a fast-forward from the implementation branch to `main`,
with release checkpoint `sparkmoe-v0.4.1-alpha`. The original branch/tag
`sparkmoe-pre-upstream-refresh` still points to `c77dd6800`. The only remaining
worktree is the original project directory. There is no remote push/publication.
Start future work from `docs/NEXT_SESSION.md`; latency tuning, platform CI,
visual/release QA and a separately authorized model integration can each run in
their own fresh conversation.

The clean UI install surfaced Mermaid and transitive nanoid audit advisories.
A scoped dependency update pins Mermaid 11.16.1 and resolves nanoid 3.3.18;
the updated lockfile audit reports zero vulnerabilities. See the
[Mermaid security advisory](https://github.com/mermaid-js/mermaid/security/advisories/GHSA-6x64-9x62-f2gx)
and [nanoid advisory](https://github.com/advisories/GHSA-2v37-7h3g-55p8).
This is dependency-audit evidence, not a complete product security review.

## Integration points

1. Public API: paging parameters, capabilities, stats, pinning and resize.
2. CLI: owned option registration/normalization and parameter propagation.
3. Model loader: metadata-only routed tensors, shard index and model ownership.
4. Context: cache lifetime, capacity and recurrent rollback microbatch guards.
5. Graph: original routing IDs preserved for scales/LoRA; slot mapping for weights.
6. CPU readiness: wait before an expert's weights are consumed by MUL_MAT_ID.
7. Server API: thin registration and owned routes/config/metrics serialization.
8. Build: owned targets, linking, embedded assets and portability.
9. Desktop/chat UI: existing product/branding/context/MTP settings.
10. Benchmark: CLI paging configuration and observable engine counters.

## Remaining manual update risks

Upstream changes to routed tensor shape/strides, graph scheduling and MUL_MAT_ID,
model/context ownership, server lifecycle, common parameter normalization and
chat UI structure can still require semantic conflict resolution. Markers cannot
prove correctness. Future model-specific integrations require resident/paged
equivalence evidence, including quantization and shard layouts.

Qwen3.8/Flash-Next-specific loading, routing and PLE graph integration are not
implemented in this phase. Generic bounded row infrastructure is preparation for
a separately validated future integration, not a model-support claim.

## Logical checkpoints

| Commit | Change |
| --- | --- |
| `b5364786d` | Two-parent upstream refresh and initial hook extraction |
| `571da9801` | Server hook isolation and safe isolated updater gates |
| `f1f1738b3` | Preserve Unicode environment during Visual Studio setup |
| `bceb510e9` | Validated direct I/O and shared demand-priority scheduling |
| `1d548eaa8` | Hybrid cache, safe resize and capability evidence |
| `418faf526` | Per-expert overlap and temporal prefetch; measured predictor experiment |
| `8a7ab7a8a` | Bounded row service, residency/UI/telemetry and recurrent MTP guard |
| `05b9573c1` | Reject unregistered upstream patches before building |
| `e55511bfb` | 0.4.0-alpha release preparation and cumulative report |
| `44cb97945` | Mermaid/nanoid security dependency updates |
| `e121a99d5` | Portable runtime, license and provenance packaging |
| `061e8657d` | 0.4.1-alpha Unicode logging fix and latency evidence |

Full commit IDs and subsequent documentation checkpoints are available in Git.
No existing history was rewritten.
