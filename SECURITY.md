# Security policy

Report suspected Spark Edge vulnerabilities through a private GitHub security advisory in this repository. Include the affected commit, platform, configuration, impact, and a minimal reproduction. Do not include real prompts, API keys, Hugging Face tokens, or private model data.

## Supported version

Only the latest tagged Spark Edge alpha is maintained. Alpha status means interfaces can change and full adversarial review has not been completed.

## Safe deployment

- The launcher binds its management API to `127.0.0.1` and starts the llama.cpp Router on localhost.
- Browser calls to the management API are restricted to the configured loopback Web UI origin. The manager does not accept arbitrary website origins.
- Do not expose either service directly to an untrusted network. If remote access is required, configure a server API key, TLS reverse proxy, firewall, and operating-system access controls.
- Treat GGUF files as untrusted input. Use files from known sources, verify hashes, and keep all split shards together.
- Do not run Spark Edge with administrator/root privileges.
- Keep model and configuration directories writable only by intended users.
- Spark Edge has no telemetry, analytics, crash upload, or automatic model download.

The launcher constructs child arguments without invoking a command shell. On Windows it uses a kill-on-close job and hidden processes so a Router restart cannot leave managed model children behind. Complete session logs are stored under the user's local Spark Edge data directory and are never uploaded. Diagnostics intentionally exclude prompts, generations, API/HF tokens, usernames, full model paths, and log contents.
