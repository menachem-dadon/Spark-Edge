#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.
"""Isolated merge attempts. Never commits, rebases or promotes a candidate."""
import argparse
import datetime
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = "config/upstream-integration.json"


def git(repo, *args):
    result = subprocess.run(["git", "-C", str(repo), *args], text=True,
                            encoding="utf-8", errors="replace", capture_output=True)
    if result.returncode:
        raise RuntimeError(f"git {' '.join(args)}: {result.stdout}{result.stderr}")
    return result.stdout.strip()


def commit(repo, ref):
    return git(repo, "rev-parse", "--verify", f"{ref}^{{commit}}")


def base(repo):
    return commit(repo, (repo / "UPSTREAM_BASE.txt").read_text().strip())


def changed(repo, before, after):
    return set(git(repo, "diff", "--name-only", before, after).splitlines())


def integration_check(repo):
    manifest = json.loads((repo / MANIFEST).read_text(encoding="utf-8"))
    errors = []
    for item in manifest["points"]:
        for filename, markers in item["files"].items():
            path = repo / filename
            if not path.is_file():
                errors.append(f"{item['id']}: missing {filename}")
                continue
            data = path.read_text(encoding="utf-8")
            for marker in markers:
                if marker not in data:
                    errors.append(f"{item['id']}: {filename}: missing {marker!r}")
    if errors:
        raise RuntimeError("Integration checks failed:\n" + "\n".join(errors))
    return manifest


def inventory(repo, revision):
    upstream = set(git(repo, "ls-tree", "-r", "--name-only", revision).splitlines())
    return sorted(upstream & set(git(repo, "diff", "--name-only", revision).splitlines()))


def check_inventory(repo, revision, manifest):
    unknown = sorted(set(inventory(repo, revision)) - set(manifest["patched_upstream_files"]))
    if unknown:
        raise RuntimeError("Unregistered upstream patches: " + ", ".join(unknown))


def target_revision(repo, target):
    if target == "latest":
        request = urllib.request.Request(
            "https://api.github.com/repos/ggml-org/llama.cpp/releases/latest",
            headers={"Accept": "application/vnd.github+json", "User-Agent": "SparkMoE-updater"})
        with urllib.request.urlopen(request, timeout=30) as response:
            release = json.load(response)
        if release["draft"] or release["prerelease"]:
            raise RuntimeError("GitHub did not return a stable release")
        target = "refs/tags/" + release["tag_name"]
    elif target == "master":
        target = "refs/heads/master"
    # Private refs avoid collisions with SparkMoE release tags.
    git(repo, "fetch", "--no-tags", "upstream", f"+{target}:refs/sparkmoe/upstream-target")
    return commit(repo, "refs/sparkmoe/upstream-target")


def state_path(repo):
    return Path(git(repo, "rev-parse", "--path-format=absolute", "--git-path", "sparkmoe-update.json"))


def save_state(repo, state):
    state_path(repo).write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")


def prepare(repo, target, destination=None):
    if git(repo, "status", "--porcelain"):
        raise RuntimeError("Working tree must be clean, including untracked files")
    if not git(repo, "branch", "--show-current"):
        raise RuntimeError("Start on a named branch")
    before = base(repo)
    if subprocess.run(["git", "-C", str(repo), "merge-base", "--is-ancestor", before, target],
                      capture_output=True).returncode:
        raise RuntimeError("Target is not a descendant of UPSTREAM_BASE.txt")
    manifest = integration_check(repo)
    check_inventory(repo, before, manifest)
    touched = sorted(set(manifest["patched_upstream_files"]) & changed(repo, before, target))
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6]
    branch, backup = "codex/upstream-" + stamp, "sparkmoe-backup/" + stamp
    candidate = Path(destination).resolve() if destination else repo.parent / (repo.name + "-upstream-" + stamp)
    if candidate.exists():
        raise RuntimeError(f"Candidate path already exists: {candidate}")
    state = {"original": str(repo), "original_head": commit(repo, "HEAD"),
             "previous": before, "target": target, "backup": backup,
             "branch": branch, "candidate": str(candidate), "touched": touched,
             "commits": int(git(repo, "rev-list", "--count", f"{before}..{target}")),
             "stage": "merge", "status": "RED"}
    print(json.dumps(state, indent=2), flush=True)
    git(repo, "config", "rerere.enabled", "true")
    git(repo, "config", "rerere.autoupdate", "false")
    git(repo, "branch", backup)
    git(repo, "tag", backup)
    git(repo, "worktree", "add", "-b", branch, str(candidate), "HEAD")
    save_state(candidate, state)
    result = subprocess.run(["git", "-C", str(candidate), "merge", "--no-commit", "--no-ff", target],
                            text=True, capture_output=True, encoding="utf-8", errors="replace")
    state["merge_output"] = result.stdout + result.stderr
    state["conflicts"] = git(candidate, "diff", "--name-only", "--diff-filter=U").splitlines()
    save_state(candidate, state)
    if result.returncode:
        raise RuntimeError(f"Merge stopped in {candidate}\n{state['merge_output']}\n"
                           f"Conflicts: {state['conflicts']}\nOriginal checkout is unchanged.")
    return candidate, state


def run_gate(repo, stage, argv, state):
    state["stage"] = stage
    save_state(repo, state)
    logs = repo / "build-upstream-gate"
    logs.mkdir(exist_ok=True)
    log = logs / (stage + ".log")
    print(f"{stage}: {argv}\n  log: {log}", flush=True)
    with log.open("w", encoding="utf-8") as output:
        result = subprocess.run([str(arg) for arg in argv], cwd=repo, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"{stage} failed ({result.returncode}); {log}\n" +
                           "\n".join(log.read_text(encoding="utf-8", errors="replace").splitlines()[-35:]))
    warnings = [line for line in log.read_text(encoding="utf-8", errors="replace").splitlines()
                if re.search(r"\bwarn(?:ing)?\b|[1-9][0-9]* vulnerabilities", line, re.IGNORECASE)]
    if warnings:
        state.setdefault("warnings", []).append({"stage": stage, "log": str(log), "examples": warnings[:10]})
        save_state(repo, state)


def validate(repo, state):
    state.update(status="RED", stage="integration")
    save_state(repo, state)
    if git(repo, "diff", "--name-only", "--diff-filter=U"):
        raise RuntimeError("Unresolved conflicts remain")
    manifest = integration_check(repo)
    check_inventory(repo, state["target"], manifest)
    state.pop("warnings", None)
    if os.name == "nt":
        command = [shutil.which("pwsh") or "powershell", "-NoProfile", "-File",
                   repo / "scripts/build-windows.ps1", "-RunTests", "-BuildRoot", repo / "build-upstream-gate/native"]
    else:
        command = ["bash", repo / "scripts/build-linux.sh", "avx2", str(repo / "build-upstream-gate/native")]
    run_gate(repo, "build-tests-ui-http", command, state)
    run_gate(repo, "updater-tests", [sys.executable, repo / "tests/test-upstream-updater.py"], state)
    (repo / "UPSTREAM_BASE.txt").write_text(state["target"] + "\n", encoding="ascii")
    state.update(status="YELLOW" if state["touched"] or state.get("warnings") else "GREEN", stage="validated")
    state["patched_upstream_files"] = inventory(repo, state["target"])
    save_state(repo, state)
    print(json.dumps(state, indent=2), flush=True)
    print("Candidate retained for review. No commit, promotion or push was performed.")
    return 2 if state["status"] == "YELLOW" else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["check", "drift", "update", "resume"])
    parser.add_argument("--repo", type=Path, default=ROOT)
    parser.add_argument("--target", default="latest", help="latest stable, master, full SHA or refs/tags/name")
    parser.add_argument("--fetch", action="store_true", help="refresh upstream/master for drift")
    parser.add_argument("--candidate", type=Path)
    args = parser.parse_args()
    repo = args.repo.resolve()
    try:
        if args.command == "check":
            manifest = integration_check(repo)
            check_inventory(repo, base(repo), manifest)
            print(f"PASS: {len(manifest['points'])} integration groups; no unregistered patches")
            return 0
        if args.command == "drift":
            if args.fetch:
                git(repo, "fetch", "--no-tags", "upstream")
            before, latest = base(repo), commit(repo, "upstream/master")
            patches = inventory(repo, before)
            print(json.dumps({"base": before, "latest_fetched": latest,
                              "commits_behind": int(git(repo, "rev-list", "--count", f"{before}..{latest}")),
                              "patched_files": patches,
                              "changed_upstream": sorted(set(patches) & changed(repo, before, latest))}, indent=2))
            return 0
        if args.command == "resume":
            state = json.loads(state_path(repo).read_text(encoding="utf-8"))
            if commit(repo, "HEAD") != state["original_head"] or str(repo) != state["candidate"]:
                raise RuntimeError("Candidate identity changed; review before starting another update")
        else:
            if git(repo, "status", "--porcelain"):
                raise RuntimeError("Working tree must be clean before fetching")
            repo, state = prepare(repo, target_revision(repo, args.target), args.candidate)
        return validate(repo, state)
    except (RuntimeError, OSError, ValueError) as error:
        print(f"RED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    sys.exit(main())
