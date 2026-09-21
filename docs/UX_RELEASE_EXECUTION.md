# SparkMoE desktop release implementation

This working tree implements the approved UI/UX changes. No commit, push, installation or public release is included.

| Workstream | Implementation | Evidence |
| --- | --- | --- |
| Manager hosts upstream UI and streams inference on a stable origin | Implemented in owned HTTP facade | Real completion, chat, SSE, disconnect and stopped-engine UI checks passed |
| Settings save/apply and revision conflicts | Implemented; running models retain their recipe until explicit reload or next load | PID continuity, deferred settings, conflicts and active-stream restart protection passed |
| Model library, short IDs, names, favourites, metadata and recommendations | Implemented; IDs survive renaming, grouped GGUF parts and legacy aliases retained | Two-part metadata, ID/path separation, persistence and recovery passed |
| Model-centric UI, basic/advanced settings and autosave | Implemented; shared model drafts, application settings and chat preferences | Svelte check passed with zero errors/warnings; latest UI bundle embedded successfully in SparkMoE.exe and llama-server.exe on 2026-09-15; rendered validation remains pending |
| Native Windows shell, tray and second launch | Implemented using pinned WebView2 SDK | Isolated visible-window, WebView2 profile, second-launch restore, stop and close checks passed |
| Agent connections | Isolated previews, files and native launch paths implemented | C++ configuration/escaping/backup/secret-isolation harness passed; real agent inference remains unverified |
| Downloads | Public Hugging Face search/catalogue, pause/resume, GGUF validation and automatic import | Live 1,185,376-byte model download, pause/resume and complete-partial recovery passed |
| Configuration recovery | Schema migration, atomic saves, backup recovery and newer-schema protection | Isolated corrupt-settings recovery and unsupported-version preservation passed |
| ZIP and installer | SDK/runtime packaging and optional signing implemented | PowerShell parse and Inno syntax checks passed; final artifacts pending |
| Upstream maintenance | Owned modules and explicit upstream patch inventory | Integration markers checked; final inventory check passed for 12 integration groups |

## Evidence

- Manager integration: `build-ux-tests.log`, `scripts/test-sparkmoe-server.py --manager-only`.
- Native lifecycle: `artifacts/test-results/native-desktop-20260914-195240/result.json`.
- Downloads: `build-ux-preview/download-result.json`.
- Connector checks: `scripts/test-sparkmoe-connections.cpp`.
- UI: focused type/Svelte checks; a broad optional lint run ran out of memory. Browser client tests initially could not start because the expected Playwright Chromium revision was absent.

## Release limits

### 2026-09-21 startup repair

- `/v1/models` now includes each model's load status, as required by the upstream chat UI, while retaining short IDs and excluding file paths. The missing status caused a JavaScript exception when a model library was present.
- The native host now synchronizes WebView2 visibility when showing, hiding, minimizing and restoring its window.
- The manager integration suite passed, including status parity between model-list endpoints and unloaded status with the engine stopped. The rebuilt application's chat rendered through WebView2 with a copy of the existing 30-model library and no missing-status exceptions.
- Native visual verification was stopped with Escape. Final on-screen visibility confirmation remains pending. Updated ZIP and installer artifacts are in `artifacts/ux-window-fix`.
- Installed binaries were updated from the verified package, with the previous binaries backed up under `artifacts/test-results/blank-window-20260921/installed-backup`. The installed manifest and release hashes match; the user's configuration file is unchanged.

The package is a local test candidate. There is no signing certificate on this machine. Full-size model performance, real coding-agent turns, clean-machine installation/uninstallation, missing-WebView2 fallback, DPI/multiple-monitor behavior and accessibility review need release qualification. Current tests use isolated data and tiny models, not the installed application or the user's chats.

Codex CLI uses process-local provider/model overrides and preserves its own instructions. Codex Desktop's native model picker is not modified. Continue and OpenCode 2 use configuration exports; Claude Code remains experimental. See `INTEGRATIONS.md`.

Browser chats stay in their original browser/origin. The native WebView2 profile is separate; existing upstream chat export/import remains the migration path. The manager's origin is stable across internal engine port changes.


