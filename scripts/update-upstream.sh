#!/usr/bin/env bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.
set -euo pipefail
exec python3 "$(dirname "$0")/sparkmoe-upstream.py" update "$@"
