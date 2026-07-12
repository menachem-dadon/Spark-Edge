# Contributing

QwenStream tracks current llama.cpp closely. Keep changes focused, modular, and easy to rebase. Read `AGENTS.md` and preserve upstream code style and licensing.

For paging changes:

1. Preserve exact inference; never add silent expert substitution or zero-weight fallback.
2. Keep platform APIs in platform-specific files.
3. Add deterministic tests for cache accounting, invalid ranges, error propagation, and parity where possible.
4. Build Release and run `test-moe-paging` on the changed platform.
5. Document unsupported behavior as an explicit error.
6. Do not commit models, tokens, generated build trees, or benchmark claims without raw configuration and measured results.

Before a pull request, run `git diff --check`, the relevant platform build script, and the paging test. Explain how real-model behavior was validated or state plainly that it was not.
