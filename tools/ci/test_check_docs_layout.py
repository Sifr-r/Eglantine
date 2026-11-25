#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

import subprocess
import sys
import unittest
from pathlib import Path


SCANNER = Path(__file__).with_name("check_docs_layout.py")

TREE = b"100644 blob aaaa\tdocs/README.md\0"
CARDS = b"100644 blob bbbb\tdocs/knowledge/core.md\0"
DIR = b"040000 tree cccc\tdocs\0"


def record(mode: str, kind: str, name: str) -> bytes:
    return f"{mode} {kind} deadbeef\t{name}\0".encode()


class DocsLayoutTests(unittest.TestCase):
    def scan(self, data: bytes):
        return subprocess.run(
            [sys.executable, "-I", str(SCANNER), "--stdin0"],
            input=data,
            capture_output=True,
            text=False,
            check=False,
        )

    def assert_rejected(self, result, *diagnostics):
        self.assertEqual(result.returncode, 1, result.stderr)
        for diagnostic in diagnostics:
            self.assertIn(diagnostic, result.stderr)

    def test_in_repo_documentation_passes(self):
        result = self.scan(DIR + TREE + CARDS)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_docs_submodule_pointer_is_rejected(self):
        result = self.scan(record("160000", "commit", "docs") + TREE + CARDS)
        self.assert_rejected(
            result,
            b"docs: documentation is a submodule pointer to another Git repository",
        )

    def test_nested_submodule_inside_docs_is_rejected(self):
        result = self.scan(DIR + TREE + record("160000", "commit", "docs/nested") + CARDS)
        self.assert_rejected(
            result,
            b"docs/nested: nested submodule pointer inside the documentation tree",
        )

    def test_missing_documentation_tree_is_rejected(self):
        result = self.scan(TREE + CARDS)
        self.assert_rejected(result, b"docs: documentation tree is missing from the repository")

    def test_missing_entry_point_and_cards_are_rejected(self):
        result = self.scan(DIR)
        self.assert_rejected(
            result,
            b"docs/README.md: documentation entry point is missing",
            b"docs/knowledge/: domain knowledge cards are missing",
        )

    def test_gitmodules_declaration_is_rejected(self):
        result = self.scan(
            DIR + TREE + CARDS + record("100644", "blob", ".gitmodules")
        )
        self.assert_rejected(
            result,
            b".gitmodules: repository declares submodules; documentation stays in-repo",
        )

    def test_incomplete_tree_input_is_rejected(self):
        self.assert_rejected(self.scan(b"100644 blob aaaa"), b"missing final NUL")

    def test_malformed_tree_record_is_rejected(self):
        self.assert_rejected(self.scan(b"not-a-tree-record\0"), b"malformed tree record")


if __name__ == "__main__":
    unittest.main()
