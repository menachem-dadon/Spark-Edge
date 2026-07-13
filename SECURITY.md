# Security policy

Report suspected QwenStream vulnerabilities through a private GitHub security advisory in this repository. Include the affected commit, platform, configuration, impact, and a minimal reproduction. Do not include real prompts, API keys, Hugging Face tokens, or private model data.

## Supported version

Only the latest tagged QwenStream alpha is maintained. Alpha status means interfaces can change and full adversarial review has not been completed.

## Safe deployment

- The launcher binds its management UI to `127.0.0.1` and starts the inference server on localhost.
- Browser calls to the management API are restricted to the control center and configured loopback chat origin. The manager does not accept arbitrary website origins.
- Do not expose either service directly to an untrusted network. If remote access is required, configure a server API key, TLS reverse proxy, firewall, and operating-system access controls.
- Treat GGUF files as untrusted input. Use files from known sources, verify hashes, and keep all split shards together.
- Do not run QwenStream with administrator/root privileges.
- Keep model and configuration directories writable only by intended users.
- QwenStream has no telemetry, analytics, crash upload, or automatic model download.

The launcher validates `.gguf` existence and constructs child arguments without invoking a command shell. The server writes a local `qwenstream-server.log` beside the launcher for troubleshooting; it is not uploaded and is ignored by Git. Diagnostics intentionally exclude prompts, generations, API/HF tokens, usernames, full model paths, and log contents.
