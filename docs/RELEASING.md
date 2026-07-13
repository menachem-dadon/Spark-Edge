# Releasing

1. Update `VERSION` and `CHANGELOG.md`.
2. Merge/rebase the desired upstream commit and update `UPSTREAM_BASE.txt`.
3. Build AVX2 and generic Windows/Linux variants without native tuning.
4. Run `npm ci`, the chat UI type check/build, unit tests, fixture parity/split tests, launcher API/dashboard smoke, server streaming, and real-model validation.
5. Confirm the portable package contains `dashboard.html`, the customized chat UI, project map, supported-model matrix, and troubleshooting guide.
6. A stable tag requires the full Qwen and Gemma resident-versus-paged gates documented in the root README and supported-model matrix. Otherwise use an `-alpha` tag.
7. Create portable archives, installer, source archive, notices, and `SHA256SUMS`.
8. Verify archives contain no model weights, compiler, source tree, tokens, or secrets.
9. Sign binaries only when the release secret/certificate is configured; clearly label unsigned artifacts.

The release workflow derives version from the tag and does not force-push.
