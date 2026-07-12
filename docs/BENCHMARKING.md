# Benchmarking

`llama-bench` accepts `--moe-paging`, `--moe-cache-mib`, `--moe-slots`, and `--moe-io-threads`. CSV and JSON/JSONL output include mode, full cache configuration, hits, misses, hit rate, bytes read, bytes/token, read count, and evictions.

Cold example:

```powershell
llama-bench.exe -m model.gguf --moe-paging explicit --moe-cache-mib 1536 --moe-io-threads 3 --no-warmup -r 1 -o json
```

Warm example removes `--no-warmup` and uses multiple repetitions. Paging stats are reset after the warmup so reported counters describe measured repetitions.

`qwenstream-iobench` measures file creation/write, sequential read, deterministic random-read IOPS, and p50/p95 read latency. Its current mode is buffered; filesystem and OS cache may dominate small tests. Use a file size larger than available RAM for device-oriented measurements and do not compare results from different sizes as if they were equivalent.

Never publish a token/s number without model identity, commit, build type, compiler, CPU, RAM, OS, and full command.
