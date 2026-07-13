# Build on Linux

```bash
./scripts/build-linux.sh avx2
./scripts/build-linux.sh generic
```

Install a C++17 compiler, CMake, Ninja or Make, Node.js 22 with npm, and standard development tools. The script uses CMake Release mode, disables native-CPU tuning, builds the customized chat UI, CLI/server/bench/I/O bench/launcher/tests, and runs the paging test. `pread` is used for expert reads. The launcher expects `llama-server` on `PATH` on POSIX in this alpha; direct CLI/server use is fully supported.

Keep `dashboard.html` in the same directory as the installed `QwenStream` executable.
