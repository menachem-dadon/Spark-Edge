# Changelog

## Unreleased

- Automatically clamp the physical microbatch to the exact MoE slot capacity instead of aborting during prompt processing.
- Report the effective microbatch through the QwenStream server configuration endpoint.

## 0.1.0-alpha - 2026-07-12

- Based the fork on llama.cpp commit recorded in `UPSTREAM_BASE.txt`.
- Added exact CPU MoE expert paging for Qwen3.5/Qwen3.6 MoE GGUF models.
- Added single/split GGUF indexing, Windows overlapped I/O, POSIX positional I/O, per-layer LRU slot caches, statistics, CLI flags, server endpoints, launcher, benchmarks, tests, and release tooling.
- Full 35B-A3B real-model validation remains a stable-release gate.
