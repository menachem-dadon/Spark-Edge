# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 SparkMoE contributors.
"""Exercise actual Git worktrees and failure paths without touching this repo."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("updater", Path(__file__).resolve().parents[1] / "scripts/sparkmoe-upstream.py")
updater = importlib.util.module_from_spec(spec)
spec.loader.exec_module(updater)


class UpdateSafety(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="sparkmoe-update-test-")
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name) / "original"
        self.repo.mkdir()
        self.g("init", "-b", "main")
        self.g("config", "user.email", "fixture@example.invalid")
        self.g("config", "user.name", "Updater fixture")
        self.write("upstream.cpp", "original\n")
        self.save()
        self.base = self.g("rev-parse", "HEAD")
        self.g("branch", "upstream")
        self.write("UPSTREAM_BASE.txt", self.base + "\n")
        self.write("upstream.cpp", "SparkMoE hook\n")
        self.write(updater.MANIFEST, json.dumps({"patched_upstream_files": ["upstream.cpp"],
                   "points": [{"id": "fixture", "files": {"upstream.cpp": ["SparkMoE hook"]}}]}))
        self.save()
        self.original = self.g("rev-parse", "HEAD")
        self.candidate = Path(self.temp.name) / "candidate"

    def g(self, *args):
        return updater.git(self.repo, *args)

    def write(self, name, data):
        path = self.repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(data, encoding="utf-8")

    def save(self):
        self.g("add", ".")
        self.g("commit", "-qm", "test fixture")

    def advance(self, conflict=False):
        self.g("switch", "upstream")
        self.write("upstream.cpp" if conflict else "new.cpp", "new upstream\n")
        self.save()
        target = self.g("rev-parse", "HEAD")
        self.g("switch", "main")
        return target

    def unchanged(self):
        self.assertEqual(self.g("rev-parse", "HEAD"), self.original)
        self.assertEqual(self.g("branch", "--show-current"), "main")
        self.assertEqual(self.g("status", "--porcelain"), "")
        self.assertEqual(updater.base(self.repo), self.base)

    def test_dirty_tree_refused(self):
        self.write("untracked.txt", "preserve me")
        with self.assertRaisesRegex(RuntimeError, "clean"):
            updater.prepare(self.repo, self.base, self.candidate)
        self.assertFalse(self.candidate.exists())

    def test_conflict_preserves_original_and_candidate(self):
        target = self.advance(conflict=True)
        with self.assertRaisesRegex(RuntimeError, "Merge stopped"):
            updater.prepare(self.repo, target, self.candidate)
        self.unchanged()
        self.assertIn("upstream.cpp", updater.git(self.candidate, "diff", "--name-only", "--diff-filter=U"))
        self.assertEqual(updater.base(self.candidate), self.base)

    def test_merge_is_not_committed_or_promoted(self):
        candidate, state = updater.prepare(self.repo, self.advance(), self.candidate)
        self.unchanged()
        self.assertEqual(updater.commit(candidate, "HEAD"), self.original)
        self.assertEqual(updater.commit(candidate, "MERGE_HEAD"), state["target"])
        self.assertEqual(self.g("rev-parse", state["backup"] + "^{commit}"), self.original)
        self.assertEqual(updater.base(candidate), self.base)

    def test_failed_validation_does_not_advance_base(self):
        candidate, state = updater.prepare(self.repo, self.advance(), self.candidate)
        with patch.object(updater, "run_gate", side_effect=RuntimeError("fixture build failure")):
            with self.assertRaisesRegex(RuntimeError, "build failure"):
                updater.validate(candidate, state)
        self.assertEqual(updater.base(candidate), self.base)
        self.assertEqual(state["status"], "RED")
        self.unchanged()

    def test_missing_hook_fails_before_build(self):
        candidate, state = updater.prepare(self.repo, self.advance(), self.candidate)
        (candidate / "upstream.cpp").write_text("hook removed\n")
        with patch.object(updater, "run_gate") as build:
            with self.assertRaisesRegex(RuntimeError, "missing"):
                updater.validate(candidate, state)
            build.assert_not_called()
        self.assertEqual(updater.base(candidate), self.base)
        self.unchanged()

    def test_success_and_review_warning_only_after_gates(self):
        candidate, state = updater.prepare(self.repo, self.advance(), self.candidate)
        state["touched"] = ["upstream.cpp"]
        with patch.object(updater, "run_gate") as gates:
            self.assertEqual(updater.validate(candidate, state), 2)
            self.assertEqual(gates.call_count, 2)
        self.assertEqual(state["status"], "YELLOW")
        self.assertEqual(updater.base(candidate), state["target"])
        self.unchanged()

    def test_unregistered_patch_fails_before_build(self):
        candidate, state = updater.prepare(self.repo, self.advance(), self.candidate)
        (candidate / "new.cpp").write_text("unregistered local change\n")
        with patch.object(updater, "run_gate") as build:
            with self.assertRaisesRegex(RuntimeError, "Unregistered"):
                updater.validate(candidate, state)
            build.assert_not_called()
        self.assertEqual(updater.base(candidate), self.base)
        self.unchanged()


if __name__ == "__main__":
    unittest.main()
