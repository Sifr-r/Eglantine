#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import catalog  # noqa: E402


class CatalogUnitTests(unittest.TestCase):
    def test_normalize_title_and_clean_validation(self):
        self.assertEqual(catalog.normalize_title(" 0400486024dec000 "), "0400486024DEC000")
        self.assertEqual(catalog.normalize_title("0400486024dec00"), "")
        self.assertEqual(catalog.normalize_title("0400486024DEC00Z"), "")
        self.assertEqual(catalog.normalize_title(None), "")

        cleaned = catalog.clean({
            "key": "  ABCDEF0123456789 ",
            "hash": "0000000000000000",
            "stage": " compute ",
            "object": "  Shadow Pass ",
            "extra_ignored": "should drop",
            "updated": "2026-11-24T00:00:00Z",
        })
        self.assertEqual(cleaned["key"], "abcdef0123456789")
        self.assertNotIn("hash", cleaned)
        self.assertEqual(cleaned["stage"], "compute")
        self.assertEqual(cleaned["object"], "Shadow Pass")
        self.assertNotIn("extra_ignored", cleaned)

        bad_ids = catalog.clean({
            "key": "not-16-hex",
            "hash": "0123456789abcdef0",
            "object": "Orphan",
        })
        self.assertNotIn("key", bad_ids)
        self.assertNotIn("hash", bad_ids)

    def test_combine_and_merge_preserve_entries_and_pick_newer_fields(self):
        repo = {
            "title": "0400486024DEC000",
            "game": "Sample Title",
            "notes": "Longer repo notes",
            "programs": [
                {
                    "key": "1111111111111111",
                    "stage": "fragment",
                    "object": "Old Name",
                    "how": "frame dump",
                    "updated": "2026-11-20T00:00:00Z",
                },
                {
                    "key": "2222222222222222",
                    "stage": "vertex",
                    "object": "Repo Only",
                    "updated": "2026-11-20T00:00:00Z",
                },
                {
                    "hash": "aaaaaaaaaaaaaaaa",
                    "stage": "compute",
                    "object": "Hash Only",
                    "updated": "2026-11-20T00:00:00Z",
                },
            ],
        }
        local = {
            "title": "0400486024DEC000",
            "game": "",
            "notes": "Short",
            "programs": [
                {
                    "key": "1111111111111111",
                    "object": "New Name",
                    "notes": "Added locally",
                    "updated": "2026-11-22T00:00:00Z",
                },
                {
                    "key": "3333333333333333",
                    "hash": "aaaaaaaaaaaaaaaa",
                    "stage": "compute",
                    "object": "Hash Match With Key",
                    "updated": "2026-11-21T00:00:00Z",
                },
            ],
        }

        merged = catalog.merge(repo, local)
        self.assertEqual(merged["game"], "Sample Title")
        self.assertEqual(merged["notes"], "Longer repo notes")
        by_key = {entry.get("key"): entry for entry in merged["programs"]}
        self.assertIn("2222222222222222", by_key)
        self.assertEqual(by_key["1111111111111111"]["object"], "New Name")
        self.assertEqual(by_key["1111111111111111"]["how"], "frame dump")
        self.assertEqual(by_key["1111111111111111"]["notes"], "Added locally")

        # Merging with an empty-timestamp hash entry matches by hash when key is absent in one side.
        hash_only = {"programs": [{"hash": "bbbbbbbbbbbbbbbb", "object": "First", "updated": "2026-11-01T00:00:00Z"}]}
        hash_upd = {"programs": [{"hash": "bbbbbbbbbbbbbbbb", "object": "Second", "notes": "Detail", "updated": "2026-11-02T00:00:00Z"}]}
        merged_hash = catalog.merge(hash_only, hash_upd)
        self.assertEqual(len(merged_hash["programs"]), 1)
        self.assertEqual(merged_hash["programs"][0]["object"], "Second")
        self.assertEqual(merged_hash["programs"][0]["notes"], "Detail")

    def test_dumps_save_and_load_roundtrip_one_program_per_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "0400486024DEC000.json"
            data = {
                "title": "0400486024DEC000",
                "game": "Test Game",
                "notes": "Note",
                "programs": [
                    {"key": "bbbbbbbbbbbbbbbb", "group": "UI", "object": "HUD", "stage": "fragment"},
                    {"key": "aaaaaaaaaaaaaaaa", "group": "Env", "object": "Sky", "stage": "vertex"},
                ],
            }
            catalog.save(str(path), data)
            text = path.read_text(encoding="utf-8")
            program_lines = [line for line in text.splitlines() if line.startswith("  {")]
            self.assertEqual(len(program_lines), 2)
            # Sorted by group ("Env" before "UI").
            self.assertEqual(json.loads(program_lines[0].rstrip(","))["key"], "aaaaaaaaaaaaaaaa")
            loaded = catalog.load(str(path))
            self.assertEqual(loaded["title"], "0400486024DEC000")
            self.assertEqual(len(loaded["programs"]), 2)

    def test_cli_status_and_merge_workflow(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            local_dir = root / "local"
            repo_dir = root / "repo"
            title = "0400486024DEC000"

            catalog.save(str(repo_dir / f"{title}.json"), {
                "title": title,
                "game": "Test Title",
                "notes": "",
                "programs": [
                    {"key": "1111111111111111", "stage": "vertex", "object": "Repo Shader",
                     "updated": "2026-11-10T00:00:00Z"},
                ],
            })
            catalog.save(str(local_dir / f"{title}.json"), {
                "title": title,
                "game": "Test Title",
                "notes": "",
                "programs": [
                    {"key": "2222222222222222", "stage": "fragment", "object": "Local Shader",
                     "updated": "2026-11-11T00:00:00Z"},
                ],
            })

            script = str(HERE / "catalog.py")
            status_before = subprocess.run(
                [sys.executable, "-I", script, "--local", str(local_dir), "--repo", str(repo_dir), "status"],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(status_before.returncode, 1, status_before.stdout + status_before.stderr)

            merged = subprocess.run(
                [sys.executable, "-I", script, "--local", str(local_dir), "--repo", str(repo_dir), "merge"],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(merged.returncode, 0, merged.stdout + merged.stderr)

            repo_after = catalog.load(str(repo_dir / f"{title}.json"))
            keys = {entry["key"] for entry in repo_after["programs"]}
            self.assertEqual(keys, {"1111111111111111", "2222222222222222"})

            status_after = subprocess.run(
                [sys.executable, "-I", script, "--local", str(local_dir), "--repo", str(repo_dir), "status"],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(status_after.returncode, 0, status_after.stdout + status_after.stderr)

    def test_local_dir_honors_env_override(self):
        with mock.patch.dict(os.environ, {"EDEN_NX2_CATALOG_DIR": "/custom/nx2-cat"}):
            self.assertEqual(catalog.local_dir(), "/custom/nx2-cat")


if __name__ == "__main__":
    unittest.main()
