# Releasing

1. Update `VERSION` and `CHANGELOG.md`.
2. Merge/rebase the desired upstream commit and update `UPSTREAM_BASE.txt`.
3. Build AVX2 and generic Windows/Linux variants without native tuning.
4. Run unit tests, fixture parity/split tests, launcher smoke, server streaming, and real-model validation.
5. A stable tag requires the full 35B-A3B gate documented in the root README. Otherwise use an `-alpha` tag.
6. Create portable archives, installer, source archive, notices, and `SHA256SUMS`.
7. Verify archives contain no model weights, compiler, source tree, tokens, or secrets.
8. Sign binaries only when the release secret/certificate is configured; clearly label unsigned artifacts.

The release workflow derives version from the tag and does not force-push.
