<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
<!-- Required Notice: Copyright (c) 2026 SparkMoE contributors. -->

# Agent connections

Spark Edge exposes a stable local API at `http://127.0.0.1:18080/v1` by default. A connection selects the model's stable API ID and a project directory. Launch waits for Spark Edge to load that model before opening the external agent. The manager must be running while the agent uses the API.

## Supported connections

| Application | Configuration | Launch | Model selection |
| --- | --- | --- | --- |
| Codex CLI | Process-local `--config` provider overrides, Responses API | Native Windows executable | Selected stable model ID with `--model`; known context window passed explicitly |
| OpenCode 1 | `OPENCODE_CONFIG_CONTENT` runtime override, OpenAI-compatible provider | Native Windows executable | Selected model and optional favourites in OpenCode's provider catalogue; `small_model` uses the selected local model |
| OpenCode 2 | Separate JSON export using the v2 provider schema | Manual | Selected model and optional favourites in the exported provider catalogue |
| Continue | Separate YAML export using an OpenAI-compatible provider | Import manually | Selected model and optional favourites in the imported configuration |
| Claude Code | Process-local Anthropic endpoint and explicit custom model ID | Experimental native Windows launch | Original local model ID, including its custom picker label; built-in background model routes also use that same ID |
| Codex Desktop | Informational guidance | Manual | Depends on the installed desktop version; Spark Edge does not change its current model picker |

OpenCode 2 is detected by the `opencode2` executable or a major-version-2 version string. Its isolated configuration loading has not been validated, so Spark Edge exports its configuration without enabling automatic launch. On Windows, `.cmd` wrappers are detected but require manual use; automated launching uses an explicit native executable with separately escaped arguments. Other operating systems can use configuration exports, but the manager does not automatically open a terminal there.

Codex CLI 0.153.4 was inspected during development. The implementation preserves its normal agent instructions, account configuration, sandbox and approval settings. A Codex startup catalogue requires agent instructions as well as model metadata. Spark Edge therefore exports the selected models to `models.json` without overriding the Codex startup catalogue or replacing its instructions. This export is a model inventory, not a Codex catalogue import file.

Claude Code support is experimental because Anthropic does not support non-Claude models. Its connector uses the local model's real ID and the documented custom-model setting. It does not rename that model to a Claude model. Compatibility with tool calls, compaction and other model-specific behaviour must be tested separately. Creating this connection requires explicit experimental opt-in.

## Files, credentials and rollback

Preview is read-only. Configure writes a new bundle under the Spark Edge data directory at `connections/<connection-id>/`. The bundle contains the provider configuration, model inventory, launch arguments and a Spark Edge record. It contains no API key. No application is installed, and no global or project application configuration is overwritten.

Updating a connection stages all files before replacing the bundle. The old bundle moves to `connections/.backups/`. Removing a connection moves its bundle to that same archive directory. The API returns the archive path, so a bundle can be restored manually to its original connection ID while Spark Edge is stopped. These archives contain only Spark Edge connection files; they are not backups of the external application's settings.

An automatic launch receives `SPARKMOE_API_KEY` from the manager through the new process's environment. Codex and OpenCode use this local credential explicitly. The Claude Code process receives the same local credential in its Anthropic authentication variables. Existing cloud credentials are never copied into the local provider configuration. Continue exports reference `${{ secrets.SPARKMOE_API_KEY }}`; supply that value through Continue's own secret configuration. The default local engine does not require a cloud account key.

The selected model alone is exported by default. Enabling favourites adds only favourites, keeping the connection inventory small. Tool capability and context length are included only when the manager knows them; unknown tool capability is shown as a warning. A successful configuration export does not establish that a model can complete an agent's tool loop.

## Manager interface

The module exports `sparkmoe::connections_dispatch(action, input, context)` from `tools/sparkmoe/connections.h`. The manager authenticates HTTP requests and supplies the data directory, current model inventory, local API endpoint, local API key and model-readiness callback.

| Action | Input | Result |
| --- | --- | --- |
| `list` | `{}` | `connectors`, saved `connections`, warnings and API base URL |
| `preview` | `connector`, `model_id`, optional `project_dir`, `include_favorites`, `experimental` | Generated `files`, argument array, non-secret environment, warnings and launch capability |
| `configure` | Same as preview; optional existing connection `id` | Preview plus saved `connection`; updating also returns `backup_path` |
| `launch` | Saved connection `id` | Process ID after model readiness and process creation; does not claim agent-turn success |
| `remove` | Saved connection `id` | Removal confirmation and `backup_path` |

## Validation

`scripts/test-sparkmoe-connections.cpp` is a standalone harness. It covers dotted model IDs, JSON/YAML generation, unknown capabilities, project validation, Windows argument round trips, experimental opt-in, configuration updates, removal archives, path traversal rejection and absence of secrets from generated files. Compile it as one C++17 translation unit with `vendor/` on the include path; link `shell32` on Windows. The harness only reads installed CLI version information and does not launch an agent or execute a model. Its temporary fixture directory is printed for inspection.

The installed Codex `--help`, `--version` and `debug models` configuration validation were inspected without changing Codex configuration or starting an inference task. The harness and a successful manager build provide configuration and integration evidence, not end-to-end agent compatibility evidence.

## Official references

- [Codex advanced configuration](https://learn.chatgpt.com/docs/config-file/config-advanced)
- [Codex configuration reference](https://learn.chatgpt.com/docs/config-file/config-reference)
- [OpenCode providers](https://opencode.ai/docs/providers)
- [OpenCode configuration precedence and runtime overrides](https://opencode.ai/docs/config/)
- [OpenCode 2 provider schema](https://opencode.ai/v2/docs/providers)
- [Continue configuration reference](https://docs.continue.dev/reference)
- [Continue OpenAI-compatible providers](https://docs.continue.dev/customize/model-providers/top-level/openai)
- [Claude Code custom model configuration](https://code.claude.com/docs/en/model-config)
