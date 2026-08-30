# Updating llama.cpp safely

SparkMoE remains the product and the only paging engine. BigMoeOnEdge is a
reference, not a dependency. New storage/cache features must preserve all routed
experts and the original top-k. Architecture-specific Flash-Next work is separate.

## One-command update

Start with a clean, committed checkout. Install Git, Python 3, Node.js 22+, CMake
and a C++ toolchain (Visual Studio C++ Build Tools on Windows).

```powershell
./scripts/update-upstream.ps1
```

The default target is GitHub's latest stable llama.cpp release. To deliberately
try master or a reviewed revision:

```powershell
./scripts/update-upstream.ps1 -Target master
./scripts/update-upstream.ps1 -Target c1d0e7a004015f23bc0233470b747b596f29b264
```

Linux: `bash scripts/update-upstream.sh` (or append `--target master`).

The script checks cleanliness before fetching, resolves the target into a private
Git ref, prints the old/new commits and count, creates a backup branch AND tag,
and creates a separate `codex/upstream-*` branch in a sibling worktree. It performs
a normal merge with `--no-commit --no-ff`; it never rewrites existing history.
Local SparkMoE and upstream release tags can have the same name, so the updater
does not fetch upstream tags over your local tags.

Validation checks integration markers, builds the product UI and all native
targets, runs paging unit/model-logit regression tests, synthetic dense/MoE HTTP
tests and updater safety tests. Only then is `UPSTREAM_BASE.txt` advanced in the
candidate. The source checkout stays on its original branch and commit.

No commit, promotion, push or PR is automated. Review the candidate and commit it
yourself after accepting warnings. Large-model tests remain a separate local
gate; the automated tests generate small GGUF files and never download models.

## GREEN / YELLOW / RED

| Result / exit code | Meaning |
| --- | --- |
| GREEN / 0 | Merge and all required local validation passed; upstream did not touch a recorded patched file. |
| YELLOW / 2 | Validation passed, but upstream touched a patched file. Review semantics even if rerere or Git merged it cleanly. |
| RED / 1 | Fetch, merge, integration check, build or regression failed. The baseline is not advanced. |

These colors are local validation results, not a claim that GitHub CI, every
model, GPU backend or performance configuration was tested. The JSON summary
includes the revisions, backup, worktree, stage, conflicts and patched-file list.
Detailed validation logs are under the candidate's `build-upstream-gate/`.
The resumable state is Git metadata (`git rev-parse --git-path sparkmoe-update.json`).

## Conflicts and rerere

The updater enables `git config rerere.enabled true` and keeps
`rerere.autoupdate false`. Git remembers resolutions but does not stage reused
resolutions automatically. Inspect both SparkMoE behavior and the new upstream
API; never accept all "ours" or "theirs". Stage reviewed resolutions with
`git add <files>`, then rerun every gate before committing:

```powershell
./scripts/update-upstream.ps1 -Resume 'C:/path/to/candidate-worktree'
```

Do not commit the pending merge before `-Resume`: it verifies the original
candidate HEAD. If a prior resolution is wrong, use `git rerere forget <file>`
while resolving the conflict, then fix it normally. A marker check cannot prove
semantic compatibility, which is why touched integration files produce YELLOW.

## Rollback

A failed attempt needs no rollback: the original checkout was never switched.
Keep the candidate to diagnose it. To abandon an unresolved merge, run
`git merge --abort` inside that candidate only. Do not delete a worktree with
uncommitted fixes. To inspect a previous version, create another worktree from
the printed `sparkmoe-backup/<timestamp>` tag. The initial refresh also preserves
`sparkmoe-pre-upstream-refresh`. Avoid force-push or reset on shared branches.

## Drift and integration points

```powershell
python scripts/sparkmoe-upstream.py check
python scripts/sparkmoe-upstream.py drift --fetch
```

`drift` reports the base, fetched master, commits behind, all patched upstream
files and which of those changed upstream. Without `--fetch` it uses local refs
and does not access the network. It compares commits, not ambiguous release tags.

When adding a hook, put the implementation in a SparkMoE-owned module, add its
file and stable symbol to `config/upstream-integration.json`, and add every newly
patched upstream file to `patched_upstream_files`. Explain its responsibility in
`UPSTREAM_INTEGRATION.md`, add a behavioral regression and run `check`.
Do not weaken a marker merely to pass an update; inspect why it disappeared.

## Local regression and performance observations

`scripts/build-windows.ps1 -RunTests` and `scripts/build-linux.sh avx2` build the
UI and all native targets, then run model-free native and HTTP gates. CI does
the same on Windows/Linux, AVX2/generic, without downloading large models.
`tests/test-upstream-updater.py` exercises clean/conflicting real Git worktrees
and injected build failures in disposable repositories.

Use existing small real models for additional paired A/B runs. Keep prompt,
seed, threads, cache budget, token count and quantization identical; record cold
and warm behavior separately. Do not fail correctness CI on small throughput
fluctuations. Full-logit equivalence and finite/nonempty generation are
correctness gates; tok/s, stalls, RAM and SSD traffic are observations.
