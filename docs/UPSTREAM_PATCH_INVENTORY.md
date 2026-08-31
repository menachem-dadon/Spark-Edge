# Maintained upstream patch inventory

Base: `c1d0e7a004015f23bc0233470b747b596f29b264`. Generated from the manifest and working-tree diff.

46 patched upstream files; 10 integration groups. SparkMoE-owned additions are excluded.

| Upstream file | Added lines | Removed lines |
| --- | ---: | ---: |
| `.gitignore` | 11 | 0 |
| `CMakeLists.txt` | 3 | 0 |
| `CONTRIBUTING.md` | 11 | 204 |
| `README.md` | 95 | 108 |
| `SECURITY.md` | 13 | 100 |
| `common/CMakeLists.txt` | 1 | 0 |
| `common/arg.cpp` | 4 | 0 |
| `common/common.cpp` | 13 | 0 |
| `common/common.h` | 10 | 0 |
| `ggml/include/ggml-cpu.h` | 4 | 0 |
| `ggml/src/CMakeLists.txt` | 4 | 0 |
| `ggml/src/ggml-cpu/arch/x86/repack.cpp` | 18 | 28 |
| `ggml/src/ggml-cpu/ggml-cpu.c` | 1 | 0 |
| `ggml/src/ggml-cpu/ggml-cpu.cpp` | 12 | 0 |
| `include/llama.h` | 94 | 0 |
| `src/CMakeLists.txt` | 3 | 0 |
| `src/llama-context.cpp` | 19 | 0 |
| `src/llama-context.h` | 7 | 0 |
| `src/llama-cparams.h` | 2 | 0 |
| `src/llama-graph.cpp` | 31 | 9 |
| `src/llama-graph.h` | 10 | 4 |
| `src/llama-model-loader.cpp` | 5 | 0 |
| `src/llama-model-loader.h` | 5 | 0 |
| `src/llama-model.cpp` | 15 | 1 |
| `src/llama-model.h` | 4 | 0 |
| `tests/CMakeLists.txt` | 14 | 0 |
| `tools/CMakeLists.txt` | 2 | 0 |
| `tools/llama-bench/llama-bench.cpp` | 160 | 3 |
| `tools/server/CMakeLists.txt` | 5 | 2 |
| `tools/server/server-context.cpp` | 4 | 0 |
| `tools/server/server-context.h` | 7 | 0 |
| `tools/server/server.cpp` | 5 | 0 |
| `tools/ui/.env.example` | 1 | 1 |
| `tools/ui/package-lock.json` | 2 | 2 |
| `tools/ui/package.json` | 2 | 2 |
| `tools/ui/pwa-assets-dark.config.ts` | 2 | 6 |
| `tools/ui/pwa-assets.config.ts` | 3 | 8 |
| `tools/ui/src/app.html` | 0 | 1 |
| `tools/ui/src/lib/components/app/misc/Logo.svelte` | 4 | 3 |
| `tools/ui/src/lib/components/app/settings/SettingsChat/SettingsChat.svelte` | 11 | 4 |
| `tools/ui/src/lib/components/app/settings/index.ts` | 3 | 0 |
| `tools/ui/src/lib/constants/app.constants.ts` | 1 | 1 |
| `tools/ui/src/lib/constants/pwa.constants.ts` | 1 | 1 |
| `tools/ui/src/lib/constants/settings.constants.ts` | 8 | 2 |
| `tools/ui/src/routes/search/+page.svelte` | 1 | 1 |
| `tools/ui/tests/e2e/pwa.e2e.ts` | 2 | 3 |

Branding, documentation and package-lock changes are included. Line counts are
a maintenance signal, not a correctness or complexity score. Use the updater
check/drift commands to regenerate the current file inventory.
