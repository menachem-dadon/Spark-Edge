# Releasing

1. Update `VERSION` and `CHANGELOG.md`.
2. Merge/rebase the desired upstream commit and update `UPSTREAM_BASE.txt`.
3. Build AVX2 and generic Windows/Linux variants without native tuning.
4. Run `npm ci`, the chat UI type check/build, unit tests, fixture parity/split tests, hidden launcher/Router API smoke, server streaming, and real-model validation.
5. Confirm the portable package contains the embedded customized chat UI, project map, supported-model matrix, and troubleshooting guide. It must not require `dashboard.html`.
6. Verify `assets/branding/SparkMoE.png` and `assets/branding/SparkMoE.ico` are final, not placeholders.
7. Verify `LICENSING.md`, the MIT license, PolyForm license, required notice, and third-party notices are included.
8. A stable tag requires the full Qwen and Gemma resident-versus-paged gates documented in the root README and supported-model matrix. Otherwise use an `-alpha` tag.
9. Create portable archives, installer, source archive, notices, and `SHA256SUMS`.
10. Perform silent install, Start menu launch without a console window, SparkMoE Web page/icon, upgrade, and uninstall smoke tests.
11. Verify archives contain no model weights, compiler, source tree, tokens, secrets, personal paths, or build logs.
12. Sign binaries only when the release secret/certificate is configured; clearly label unsigned artifacts.

The release workflow derives version from the tag and does not force-push.
