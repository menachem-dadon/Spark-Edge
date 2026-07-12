#!/usr/bin/env bash
set -euo pipefail
strategy="${1:-rebase}"
[[ "$strategy" == rebase || "$strategy" == merge ]] || { echo "usage: $0 [rebase|merge]" >&2; exit 2; }
root="$(cd "$(dirname "$0")/.." && pwd)"; cd "$root"
branch="$(git branch --show-current)"; [[ -n "$branch" ]] || { echo "run on a named branch" >&2; exit 1; }
git fetch upstream
backup="qwenstream-backup/$(date -u +%Y%m%d-%H%M%S)"; git branch "$backup"; echo "Backup branch: $backup"
if [[ "$strategy" == rebase ]]; then git rebase upstream/master; else git merge --no-ff upstream/master; fi
git rev-parse upstream/master > UPSTREAM_BASE.txt
"$root/scripts/build-linux.sh" avx2
