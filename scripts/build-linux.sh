#!/usr/bin/env bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.

set -euo pipefail
variant="${1:-avx2}"
case "$variant" in avx2|generic) ;; *) echo "usage: $0 [avx2|generic]" >&2; exit 2;; esac
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${2:-$root/build-linux-$variant}"
command -v npm >/dev/null || { echo "Node.js 22 and npm are required to build the SparkMoE chat UI" >&2; exit 1; }
unset NODE_TLS_REJECT_UNAUTHORIZED
export NODE_USE_SYSTEM_CA=1
(cd "$root/tools/ui" && npm ci && npm run check && npm run build)
args=(-S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=OFF -DLLAMA_BUILD_UI=OFF -DLLAMA_USE_PREBUILT_UI=OFF -DLLAMA_BUILD_SERVER=ON -DLLAMA_BUILD_TESTS=ON)
if [[ "$variant" == generic ]]; then
  args+=(-DGGML_AVX=OFF -DGGML_AVX2=OFF -DGGML_FMA=OFF -DGGML_F16C=OFF -DGGML_BMI2=OFF)
else
  args+=(-DGGML_AVX=ON -DGGML_AVX2=ON)
fi
cmake "${args[@]}"
cmake --build "$build" --parallel 4
ctest --test-dir "$build" --output-on-failure -R '^test-moe-'
python3 "$root/scripts/test-sparkmoe-server.py" --bin-dir "$build/bin" --log-dir "$build/server-smoke"
