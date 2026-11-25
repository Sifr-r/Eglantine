#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

import glob
import os
import re
import unittest
import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORKFLOWS_DIR = os.path.join(ROOT, ".github", "workflows")
ACTIONS_DIR = os.path.join(ROOT, ".github", "actions")


def load_yaml(path):
    with open(path, encoding="utf-8") as handle:
        data = yaml.safe_load(handle)
    if not isinstance(data, dict):
        raise AssertionError(f"Expected YAML mapping in {path}")
    return data


def workflow_triggers(data):
    # PyYAML parses unquoted top-level YAML `on:` keys as boolean True in YAML 1.1 mode.
    if "on" in data:
        return data["on"]
    return data.get(True)


class WorkflowConfigurationTests(unittest.TestCase):
    def setUp(self):
        self.workflow_paths = sorted(glob.glob(os.path.join(WORKFLOWS_DIR, "*.yml")))
        self.action_paths = sorted(glob.glob(os.path.join(ACTIONS_DIR, "*", "*.yml")))
        self.workflows = {os.path.basename(p): load_yaml(p) for p in self.workflow_paths}
        self.actions = {
            os.path.relpath(p, ACTIONS_DIR): load_yaml(p) for p in self.action_paths
        }

    def test_all_workflows_and_actions_parse_as_mappings(self):
        self.assertGreater(len(self.workflows), 0)
        self.assertGreater(len(self.actions), 0)
        for name, data in self.workflows.items():
            self.assertIn("jobs", data, f"{name} is missing jobs")

    def test_all_non_reusable_jobs_declare_positive_timeouts(self):
        for name, data in self.workflows.items():
            jobs = data["jobs"]
            self.assertIsInstance(jobs, dict, name)
            for job_id, job in jobs.items():
                self.assertIsInstance(job, dict, f"{name}:{job_id}")
                if "uses" in job:
                    continue
                timeout = job.get("timeout-minutes")
                self.assertIsInstance(
                    timeout,
                    int,
                    f"{name}:{job_id} must set integer timeout-minutes",
                )
                self.assertFalse(
                    isinstance(timeout, bool),
                    f"{name}:{job_id} timeout-minutes must not be boolean",
                )
                self.assertGreater(timeout, 0, f"{name}:{job_id} timeout must be > 0")

    def test_no_stale_repo_pins_debug_artifacts_or_typos(self):
        for path in self.workflow_paths:
            with open(path, encoding="utf-8") as handle:
                text = handle.read()
            name = os.path.basename(path)
            self.assertNotIn("Djihads80/Eglantine", text, name)
            self.assertNotIn("ccache-debug", text, name)
            self.assertNotIn("obselete-", text, name)

    def test_ci_triggers_reusable_tests_and_post_release_cleanup(self):
        ci = self.workflows["ci.yml"]
        triggers = workflow_triggers(ci)
        self.assertIn("opened", triggers["pull_request"]["types"])
        kind_input = triggers["workflow_dispatch"]["inputs"]["kind"]
        self.assertIn("release", kind_input["options"])
        self.assertEqual(ci["jobs"]["tests"]["uses"], "./.github/workflows/test-linux.yml")
        self.assertEqual(
            ci["jobs"]["mark-obsolete"]["uses"],
            "./.github/workflows/obsolete-on-release.yml",
        )

    def test_source_checks_runs_on_push_and_uses_cached_linux_tests(self):
        sc = self.workflows["source-checks.yml"]
        triggers = workflow_triggers(sc)
        self.assertIn("nx2", triggers["push"]["branches"])
        self.assertIn("nx2-experimental", triggers["push"]["branches"])
        self.assertIn("workflow_dispatch", triggers)
        self.assertEqual(sc["jobs"]["tests"]["uses"], "./.github/workflows/test-linux.yml")
        lh = sc["jobs"]["license-headers"]
        self.assertEqual(lh["if"], "github.event_name == 'pull_request'")
        self.assertIn("tools/ci/license-header.sh", lh["steps"][1]["run"])

    def test_source_checks_verifies_the_documentation_layout(self):
        sc = self.workflows["source-checks.yml"]
        job = sc["jobs"]["docs-layout"]
        run = next(step["run"] for step in job["steps"] if "run" in step)
        self.assertIn("tools/ci/check_docs_layout.py", run)
        self.assertIn("--ref HEAD", run)

    def test_test_linux_runs_license_header_tests_and_shellcheck(self):
        tl = self.workflows["test-linux.yml"]
        steps = tl["jobs"]["scripts"]["steps"]
        py_step = next(s for s in steps if s.get("name") == "Run Python unit tests")
        sc_step = next(s for s in steps if s.get("name") == "Run ShellCheck on CI and NX2 scripts")
        self.assertIn("tools/ci/test_license_header.py", py_step["run"])
        self.assertIn("tools/ci/test_check_docs_layout.py", py_step["run"])
        self.assertIn("tools/test_dead_code.py", py_step["run"])
        self.assertIn("tools/nx2/test_catalog.py", py_step["run"])
        self.assertIn("tools/ci/license-header.sh", sc_step["run"])

        catch2_steps = tl["jobs"]["catch2"]["steps"]
        gpui_step = next(s for s in catch2_steps if s.get("name") == "Run GPUI library unit tests")
        self.assertIn("rust/apps/eglantine_gpui/Cargo.toml", gpui_step["run"])
        self.assertIn("--no-default-features", gpui_step["run"])

    def test_windows_mingw_pgo_receives_display_pgo(self):
        bw = self.workflows["build-windows.yml"]
        setup_step = next(
            step
            for step in bw["jobs"]["mingw-pgo"]["steps"]
            if step.get("name") == "Setup build from checked-out source"
        )
        self.assertEqual(setup_step["with"]["display"], "${{ inputs.display_pgo }}")

    def test_obsolete_on_release_triggers_and_exemptions(self):
        oor = self.workflows["obsolete-on-release.yml"]
        triggers = workflow_triggers(oor)
        self.assertIn("release", triggers)
        self.assertIn("workflow_call", triggers)
        self.assertIn("workflow_dispatch", triggers)
        with open(os.path.join(WORKFLOWS_DIR, "obsolete-on-release.yml"), encoding="utf-8") as handle:
            text = handle.read()
        self.assertIn("-label:pinned", text)
        self.assertIn("no:assignee", text)
        self.assertIn("no:milestone", text)

    def test_retarget_prs_exempts_release_branches(self):
        retarget = self.workflows["retarget_prs.yml"]
        condition = retarget["jobs"]["retarget"]["if"]
        self.assertIn("release/v", condition)

    def test_release_notes_escapes_backticks_in_macos_row(self):
        path = os.path.join(ROOT, "tools", "ci", "release-notes.sh")
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        macos_lines = [line for line in text.splitlines() if "arm64 (Apple Silicon)" in line]
        self.assertEqual(len(macos_lines), 1)
        line = macos_lines[0]
        self.assertIn("\\`xattr -dr com.apple.quarantine eglantine.app\\`", line)
        self.assertIn("\\`eden-cli\\`", line)
        self.assertIn("\\`Contents/MacOS\\`", line)
        self.assertIsNone(re.search(r"(?<!\\)`", line.split('"', 1)[1]))


if __name__ == "__main__":
    unittest.main()
