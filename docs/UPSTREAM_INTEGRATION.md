# Spark Edge upstream integration

## Desktop manager ownership (2026-09-14)

The stable manager UI origin, HTTP facade, model library, and native Windows shell live in Spark Edge modules under `tools/sparkmoe/`. The desktop-manager manifest group checks the STA host, restricted navigation/download surface, CMake loader integration, pinned SDK restore, and installer Runtime/PATH behavior. Preserve these modules during an upstream refresh and keep shared chat changes scoped to existing UI integration points.

The SDK pin is `packaging/windows/webview2-sdk.json`. SDK sources, loader hashes, notices, and package signature state are recorded by the Windows scripts. The current native and package validation requirements are in `WEBVIEW2_DESKTOP_INTEGRATION_PLAN.md`; historical refresh evidence below does not validate this new desktop implementation.

## Audit before the refresh (2026-08-31)

- Original upstream: `6b4dc2116a92c5c8f2782bfe51fabe5ee66fb5ef`.
- Verified with `git merge-base HEAD upstream/master`, not only UPSTREAM_BASE.txt.
- Fork tip: `c77dd6800` (13 commits reachable beyond the original base,
  including the initial import/merge).
- Recovery branch and tag: `sparkmoe-pre-upstream-refresh`.
- Work branch: `codex/upstream-refresh-v0.3.0`.
- Selected target: `v0.3.0`, `c1d0e7a004015f23bc0233470b747b596f29b264`,
  the latest official stable release on 2026-08-31 (644 upstream commits).
  CPU Windows and Linux upstream checks passed. The optional Windows OpenBLAS
  job failed; this CPU build does not enable BLAS. Upstream master was another
  90 commits ahead. No new architecture-specific Spark Edge support is in scope.

## Ownership and pre-merge diff

The original comparison contains 108 files, 5,549 insertions and 944 deletions.
Documentation replacements and branding account for much of this count.

| Category | Upstream files patched before refresh | Responsibility |
| --- | --- | --- |
| Public API | `include/llama.h` | Existing paging parameters, capabilities, stats and pinning API |
| CLI/options | `common/arg.cpp`, `common/common.cpp`, `common/common.h` | Flags, defaults, auto fallback, propagation to model/context |
| Model loading | `src/llama-model-loader.cpp`, `src/llama-model-loader.h`, `src/llama-model.cpp`, `src/llama-model.h`, `src/models/qwen35moe.cpp` | Metadata-only expert tensors, file index ownership, layer selection |
| Context/graph | `src/llama-context.cpp`, `src/llama-context.h`, `src/llama-cparams.h`, `src/llama-graph.cpp`, `src/llama-graph.h` | Per-context cache lifetime, safe microbatch, router-to-slot mapping, original IDs for scales/LoRA |
| Server | `tools/server/server-context.cpp`, `tools/server/server-context.h`, `tools/server/server.cpp` | Four existing Spark Edge status/config/statistics routes |
| Benchmark | `tools/llama-bench/llama-bench.cpp` | Paging options and observable cache/I/O metrics |
| CMake/build | `CMakeLists.txt`, `src/CMakeLists.txt`, `ggml/src/CMakeLists.txt`, `tools/CMakeLists.txt`, `tools/server/CMakeLists.txt`, `tests/CMakeLists.txt` | Spark Edge targets and Windows build compatibility |
| CPU compatibility | `ggml/src/ggml-cpu/arch/x86/repack.cpp` | Avoid passing AVX vectors across the MinGW helper ABI |
| Chat UI | `tools/ui/.env.example`, `tools/ui/package.json`, `tools/ui/package-lock.json`, PWA/branding files, `tools/ui/src/lib/constants/routes.constants.ts`, `tools/ui/src/lib/constants/ui.constants.ts`, `tools/ui/src/lib/components/app/navigation/SidebarNavigation/SidebarNavigation.svelte`, `tools/ui/src/routes/search/+page.svelte` | Existing branding plus the Spark Edge sidebar integration |
| Other | `.gitignore`, `README.md`, `CONTRIBUTING.md`, `SECURITY.md` | Fork documentation and generated-artifact hygiene |

Spark Edge core is `src/moe-paging/`: the model index, file readers, bounded
per-layer cache, paging manager and statistics. These algorithms are independent
of the integration hooks. The hidden launcher/Router supervisor (`tools/sparkmoe/`), I/O tool,
branding, presets, packaging, scripts, paging tests and dedicated Web page are
also part of Spark Edge. Preserve their existing behavior and licensing.

## Refresh strategy

Use a normal two-parent merge, without rewriting either history. The user's
implementation instruction authorizes logical local commits after verification;
include an Assisted-by trailer. Do not push or submit upstream contributions.
Keep upstream model implementations intact where the common tensor-creation hook
already handles the same work. Move coherent integration implementations into
owned modules, not textual copies of upstream algorithms.

The maintenance manifest and final validation report will record the resulting
integration points, conflict resolutions, remaining patched files and evidence.

## Revised execution plan: one Spark Edge engine

The user's decision supersedes the backend recommendation in the existing
`C:/codex-output/sparkmoe-bmoe-research-20260831/DECISION_REPORT.md`.
Keep its measured mechanisms and limitations as reference evidence. Do not add
BigMoeOnEdge as a dependency, backend or second paging engine. Preserve the
existing cache, partial layers, expert pinning, Unicode paths, full llama-server,
chat UI, manager, MTP and context settings.

| Checkpoint | Work | Acceptance before proceeding | Status |
| --- | --- | --- | --- |
| R1 | Finish the frozen v0.3.0 refresh; restore build, model/graph equivalence and existing UI/server behavior | CPU build, paging unit tests, finite full-logit fixture parity, dense/MoE smoke, UI check/build | Passed on Windows |
| R2 | Finish owned integration modules, manifest, safe updater, drift checker, CI and rollback docs | Clean candidate worktree, integration checks, failure-path updater tests, build/regression gate | Windows build/HTTP and 6 updater safety tests passed; hosted CI not run |
| E1 | Aligned Windows/Linux direct reads, byte validation, buffered fallback; demand-priority worker scheduling | Buffered/direct byte and logits parity, Unicode/shards/tail/error checks; paired local I/O observations | Passed on Windows; measurements in ENGINE_UPGRADES.md |
| E2 | Hybrid global budget over per-layer pools; runtime resize, capacity/eviction telemetry, capability registry | Preserve pins and partial layers; resize only at quiescent boundaries; budget/error and full-logit parity | Passed on Windows; defaults unchanged |
| E3 | Per-expert readiness and compute/I/O overlap in the same cache; temporal prefetch behind an option | Demand always wins over queued speculation; no early weight consumption; overlap/prefetch parity and paired A/B | Passed on Windows with real cache pressure |
| E4 | Dense/shared residency policy and generic bounded row streaming; Windows RAM sensors | Bounded residency, finite row-gather parity, Unicode/multi-shard reads; no huge-tensor materialization fallback | Windows native/HTTP/Desktop gates passed |
| E5 | Predictive prefetch evaluation | Experimental only; retain only when paired benchmarks show useful reads and an actual benefit | Evaluated and rejected as slower; prototype archived, excluded from product |

All modes must preserve original routing/top-k and every routed expert. No
dropping, substitution or quality-changing optimization is part of this plan.
New mechanisms remain opt-in until correctness and measurements justify defaults.
Performance observations are recorded separately from deterministic correctness
gates. Use short paired runs on existing local models, not downloaded large models
or broad benchmarks after every small edit. Each major checkpoint gets a tested
local commit; failed experiments must not be reported as completed features.

Generic row streaming is infrastructure only at this stage. A table must have a
validated gather access contract and bounded in-flight rows; an unsupported access
fails explicitly instead of loading an enormous table into RAM. Do not register
Flash-Next PLE or introduce Qwen3.8-specific graph/model code. That remains a
separate follow-up after these checkpoints are stable.

### R1 evidence (2026-08-31)

- Full MSVC Release CPU build, including tools, examples and tests: PASS.
- Paging unit test and synthetic GGUF full-logit regression: 2/2 PASS.
  Dense/off, explicit, OS, auto, partial layers, pinning, eviction and unload
  are checked without downloading a model. All vocabulary logits are compared.
- HTTP dense and MoE completions/chat, telemetry and reset: PASS, using generated
  fixtures in a Windows Hebrew path. Legacy manager UI/config smoke: PASS.
- Svelte check: zero errors/warnings. Production chat UI build: PASS, embedded
  in the rebuilt server. This does not establish browser interaction parity.
- Existing local Ling-3.0-tiny Q4_K_M, CPU 4 threads, 1,500 MiB explicit cache,
  tg8 x 2: PASS; 6.19 and 11.23 tok/s, 1,063 cache misses and 1.52 GB logical
  reads. Warm-cache variation is an observation, not a performance gate.
- Linux was not built locally: WSL has Python but no CMake/C++ toolchain.
  CI remains necessary for Linux validation.

Resolved conflicts covered upstream load-mode APIs, common_json, graph/model
loading, server routing, CMake and the reorganized UI settings/PWA files.
MSVC validation additionally fixed legacy manager UI string limits, Unicode resource
paths, private unit-test linkage and benchmark paging parameter initialization.

The local Spark Edge tag `v0.3.0` names a different commit from upstream's tag of
the same name. Always use the full upstream SHA or a private fetched ref; never
infer the upstream base from an unqualified local release tag.

### R2 integration boundary

The executable inventory is `config/upstream-integration.json`. It records every
patched upstream file, including branding/build portability, and ten hook groups:
public API, CLI, model loading, context lifetime, graph routing, server API,
build, desktop UI, benchmark and CPU expert readiness. `scripts/sparkmoe-upstream.py check` detects
missing symbols/files and unregistered upstream patches. `drift` compares the
whole inventory against fetched upstream history.

Integration implementations now live in `common/sparkmoe.*`,
`src/moe-paging/moe-integration.*` and `tools/sparkmoe/server-api.*`. These remain
MIT because they were extracted from MIT upstream patches. Core algorithms stay
under their existing license. No BigMoeOnEdge source or runtime is imported.

Within the upstream-derived `src/`, `common/`, `include/` and `tools/server/`
files, the integration changed from 525 insertions/21 deletions in 19 files to
203 insertions/16 deletions in 19 files, before engine enhancements. This measures
the upstream patch surface, not total project code. Most remaining lines are
public structures, original-ID preservation for scales/LoRA, and lifecycle hooks.
The Qwen3.5-specific tensor-loader patch was removed in favor of the common hook.

### Engine additions

The maintained inventory now contains 47 upstream files. E3 adds only 17 lines
across `ggml/include/ggml-cpu.h`, `ggml/src/ggml-cpu/ggml-cpu.cpp` and
`ggml/src/ggml-cpu/ggml-cpu.c`: a callback declaration/dispatch and one wait before
each used expert in MUL_MAT_ID. Its readiness tables, scheduling, cache leases
and error handling belong to Spark Edge. Public parameter/stat fields, benchmark
flags and Windows server memory linking account for the other justified growth.

The Desktop group also tracks the one-line `common/log.cpp` change from `fopen`
to upstream's existing `ggml_fopen`. Without it, native logs silently fail under
Hebrew Windows installation paths. Desktop's own tail reader uses `u8path`.
The final src/common/include/server patch is 245 insertions/17 deletions across
20 files, including that portability correction.

Direct I/O, scheduling, hybrid commitment, eviction, prefetch, row streaming and
capability evidence remain under `src/moe-paging/`. Model/context initialization
also guards the upstream recurrent rollback microbatch contract; hybrid resize
cannot invalidate that contract. No model-family implementation was added.

Sensitive future updates include routed tensor layouts, graph scheduling and
MUL_MAT_ID, model/context ownership, public parameter structs, common argument
normalization, server lifecycle and UI settings structure. Markers are an early
warning, not a substitute for model equivalence tests or manual semantic review.
