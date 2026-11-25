#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

"""Check that the documentation tree lives in this repository, not in another one.

Eglantine's documentation is part of the repository. It used to be a submodule
pointing at a separate docs repository, which made every edit depend on a second
clone and let an upstream merge reinstate the pointer. This check reads the tree
and fails when the documentation is anything but an ordinary directory here.
"""

import argparse
import sys

DOCS = "docs"
GITLINK_MODE = "160000"
TREE_MODE = "040000"


def parse_records(raw: bytes) -> list[tuple[str, str]]:
    """Split NUL-terminated `git ls-tree` records into (mode, name) pairs."""
    records = []
    for chunk in raw.split(b"\0"):
        if not chunk:
            continue
        text = chunk.decode("utf-8", errors="surrogateescape")
        meta, separator, name = text.partition("\t")
        fields = meta.split()
        if not separator or len(fields) < 3:
            raise ValueError(f"malformed tree record: {ascii(text)}")
        records.append((fields[0], name))
    return records


def problems_in(records: list[tuple[str, str]]) -> list[str]:
    """Return one diagnostic per way the tree breaks the in-repo documentation rule."""
    modes = {name: mode for mode, name in records}
    problems = []

    for mode, name in records:
        if mode == GITLINK_MODE and name.startswith(DOCS + "/"):
            problems.append(f"{name}: nested submodule pointer inside the documentation tree")

    docs_mode = modes.get(DOCS)
    if docs_mode is None:
        problems.append(f"{DOCS}: documentation tree is missing from the repository")
    elif docs_mode == GITLINK_MODE:
        problems.append(f"{DOCS}: documentation is a submodule pointer to another Git repository")
    elif docs_mode != TREE_MODE:
        problems.append(f"{DOCS}: documentation is not a regular directory (mode {docs_mode})")

    if f"{DOCS}/README.md" not in modes:
        problems.append(f"{DOCS}/README.md: documentation entry point is missing")

    if not any(name.startswith(f"{DOCS}/knowledge/") for _, name in records):
        problems.append(f"{DOCS}/knowledge/: domain knowledge cards are missing")

    if ".gitmodules" in modes:
        problems.append(".gitmodules: repository declares submodules; documentation stays in-repo")

    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--stdin0",
        action="store_true",
        help="read NUL-terminated `git ls-tree -r -t -z` records from stdin",
    )
    parser.add_argument(
        "--ref",
        help="read the tree of this ref instead of stdin (for example HEAD)",
    )
    args = parser.parse_args()

    if args.stdin0 == bool(args.ref):
        parser.error("give exactly one of --stdin0 or --ref")

    if args.stdin0:
        raw = sys.stdin.buffer.read()
        if raw and not raw.endswith(b"\0"):
            print("Git tree: missing final NUL", file=sys.stderr)
            return 1
    else:
        import subprocess

        completed = subprocess.run(
            ["git", "ls-tree", "-r", "-t", "-z", args.ref],
            capture_output=True,
            check=False,
        )
        if completed.returncode != 0:
            print(f"Git tree: cannot read {args.ref}: {completed.stderr.decode(errors='replace').strip()}", file=sys.stderr)
            return 1
        raw = completed.stdout

    try:
        records = parse_records(raw)
    except ValueError as error:
        print(f"Git tree: {error}", file=sys.stderr)
        return 1

    problems = problems_in(records)
    for problem in problems:
        print(f"Git tree: {problem}", file=sys.stderr)
    return int(bool(problems))


if __name__ == "__main__":
    sys.exit(main())
