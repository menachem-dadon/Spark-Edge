# Build on Linux

```bash
./scripts/build-linux.sh avx2
./scripts/build-linux.sh generic
```

The script uses CMake Release mode, disables native-CPU tuning, builds CLI/server/bench/I/O bench/launcher/tests, and runs the paging test. `pread` is used for expert reads. The launcher expects `llama-server` on `PATH` on POSIX in this alpha; direct CLI/server use is fully supported.
