# Licensing map

GiantLLM is a multi-license repository. The repository-level `LICENSE` is the upstream llama.cpp MIT license. It remains in force for upstream code and for modifications made directly inside upstream-derived files.

The original GiantLLM components listed below are offered under the PolyForm Noncommercial License 1.0.0 in `LICENSE-GIANTLLM-NONCOMMERCIAL.md`:

- `assets/branding/`
- `config/presets/`
- `src/moe-paging/`
- `tools/giantllm/`
- `tools/giantllm-iobench/`
- `packaging/`
- `scripts/build-installer.ps1`
- `scripts/build-linux.sh`
- `scripts/build-windows.ps1`
- `scripts/diagnostics.ps1`
- `scripts/package-windows.ps1`
- `scripts/update-upstream.ps1`
- `scripts/update-upstream.sh`
- `scripts/validate-real-model.ps1`
- `.github/workflows/giantllm-ci.yml`
- `.github/workflows/giantllm-release.yml`
- `tests/test-moe-paging.cpp`
- `tools/ui/src/lib/components/app/settings/SettingsChat/SettingsChatServerTab.svelte`
- GiantLLM-specific documentation created in this fork, except text copied from upstream.

The following remain MIT even when they contain GiantLLM integration hooks, because they are upstream-derived files and separating a patch-level license inside them would be ambiguous:

- `include/llama.h`
- `common/`
- upstream files under `src/` outside `src/moe-paging/`
- upstream files under `tools/server/`, `tools/ui/`, and `tools/llama-bench/`
- upstream CMake files, tests, examples, conversion tools, and GGML code

Third-party components keep their own licenses as documented in `THIRD_PARTY_NOTICES.md`. Model files are not distributed and have separate terms described in `docs/MODEL_LICENSES.md`.

Commercial use of the PolyForm-covered components requires a separate license from the GiantLLM copyright holders. This file is an engineering ownership map, not legal advice. Before public release, replace the generic required notice with the legal name or entity that actually owns the original work and have counsel review the boundary.
