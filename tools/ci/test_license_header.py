#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

import datetime
import glob
import os
import shutil
import stat
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPT = os.path.join(ROOT, "tools", "ci", "license-header.sh")
HOLDER = "Eglantine Emulator Project"
LICENSE = "GPL-3.0-or-later"

REPAIRED_SPDX_FILES = [
    "flake.nix",
    "nix/package.nix",
    "nix/build-eden-emulator.nix",
    "tools/mcp/scripts/mcp.py",
    "tools/nx2/catalog.py",
    "tools/nx2/debug_mcp.py",
    "tools/nx2/lab_ctl.py",
    "tools/nx2/shader_lab.py",
    "tools/nx2/test_debug_mcp.py",
    "tools/nx2/test_thread_worker_wake.py",
    "tools/nx2/thread_worker_wake.py",
    "tools/ci/license-header.sh",
    "tools/ci/test_license_header.py",
    "src/core/hle/service/set/key_code_map.h",
    "src/tests/core/key_code_map.cpp",
]

POLYGLOT_SPDX_GLOBS = [
    "CMakeModules/AddIspcKernel.cmake",
    "CMakeModules/AddRustCrate.cmake",
    "rust/crates/*/src/**/*.rs",
    "rust/crates/*/tests/*.rs",
    "rust/include/*.h",
    "src/simd_kernels/CMakeLists.txt",
    "src/simd_kernels/*.ispc",
    "src/video_core/host_shaders/*.slang",
    "src/video_core/renderer_vulkan/vk_temporal_upscaler.h",
    "src/video_core/renderer_vulkan/vk_xess_upscaler.h",
    "src/video_core/renderer_vulkan/vk_xess_upscaler.cpp",
    "src/tests/video_core/ampere_rust_parity.cpp",
    "src/tests/video_core/ampere_direct_spirv.cpp",
]


def comment_prefix(path):
    name = os.path.basename(path)
    if name.endswith((".cmake", ".sh", ".ps1", ".py", ".rb", ".perl", ".pl", ".nix")) or name == "CMakeLists.txt":
        return "#"
    return "//"


def validate_spdx_header(lines, begin, year):
    """Mirror the leading-header SPDX validation contract of license-header.sh."""
    expected_cp = f"{begin} SPDX-FileCopyrightText: Copyright {year} {HOLDER}"
    expected_lic = f"{begin} SPDX-License-Identifier: {LICENSE}"
    spdx_cp_prefix = f"{begin} SPDX-FileCopyrightText:"
    spdx_lic_prefix = f"{begin} SPDX-License-Identifier:"

    stanza_cp = False
    stanza_lic = False
    stanza_egl_cp = False
    stanza_last_was_lic = False
    has_eglantine_stanza = False
    in_c_comment = False

    def reset_stanza():
        nonlocal stanza_cp, stanza_lic, stanza_egl_cp, stanza_last_was_lic
        if stanza_cp and not stanza_lic:
            raise AssertionError("Orphaned copyright stanza without SPDX-License-Identifier")
        stanza_cp = False
        stanza_lic = False
        stanza_egl_cp = False
        stanza_last_was_lic = False

    for idx, raw in enumerate(lines):
        line = raw.rstrip("\r\n")
        if idx == 0 and line.startswith("#!"):
            continue
        if in_c_comment:
            if "*/" in line:
                in_c_comment = False
            continue
        if not line.strip():
            reset_stanza()
            continue
        if begin == "//" and line.lstrip().startswith("/*"):
            if "*/" not in line:
                in_c_comment = True
            continue
        if line.startswith(begin):
            has_inline_cp = ("SPDX-FileCopyrightText:" in line) or ("Copyright" in line)
            is_spdx_lic = line.startswith(spdx_lic_prefix) or (
                has_inline_cp and "SPDX-License-Identifier:" in line
            )
            if is_spdx_lic:
                if stanza_last_was_lic and not has_inline_cp:
                    raise AssertionError(f"Consecutive SPDX-License-Identifier without copyright: {line}")
                stanza_lic = True
                stanza_last_was_lic = True
                if stanza_egl_cp and line == expected_lic:
                    has_eglantine_stanza = True
            if line.startswith(spdx_cp_prefix) or has_inline_cp:
                if line == expected_cp:
                    stanza_egl_cp = True
                stanza_cp = True
                stanza_last_was_lic = False
            continue
        reset_stanza()
        break
    else:
        reset_stanza()

    if not has_eglantine_stanza:
        raise AssertionError("Missing valid Eglantine SPDX copyright/license stanza in leading header")


def has_working_posix_shell():
    if shutil.which("sh") is None:
        return False
    try:
        res = subprocess.run(
            ["sh", "-c", "awk 'BEGIN { exit 0 }' && mktemp -d | xargs rm -rf"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        return res.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


class LicenseHeaderContractTests(unittest.TestCase):
    def setUp(self):
        with open(SCRIPT, encoding="utf-8") as handle:
            self.script_text = handle.read()
        self.year = str(datetime.date.today().year)

    def test_script_fixes_base_branch_exclusions_shebang_chmod_and_exit_code(self):
        self.assertIn('BASE_REF="nx2"', self.script_text)
        self.assertNotIn("git merge-base master HEAD", self.script_text)
        self.assertIn("externals/FidelityFX-SDK", self.script_text)
        self.assertNotIn("externals/FidelityFX-FSR", self.script_text)
        self.assertNotIn("chmod a+x", self.script_text)
        self.assertIn("trap cleanup EXIT INT TERM", self.script_text)
        self.assertIn("exit 1", self.script_text)
        self.assertIn("'#!'*", self.script_text)
        self.assertIn("*.rs|*.slang|*.ispc|*.slint", self.script_text)

    def test_polyglot_sources_have_valid_spdx_headers(self):
        matched = []
        for pattern in POLYGLOT_SPDX_GLOBS:
            matched.extend(sorted(glob.glob(os.path.join(ROOT, pattern), recursive=True)))
        self.assertGreater(len(matched), 0)
        for path in matched:
            rel = os.path.relpath(path, ROOT)
            with self.subTest(path=rel):
                with open(path, encoding="utf-8") as handle:
                    lines = handle.readlines()
                validate_spdx_header(lines, comment_prefix(rel), "2026")

    def test_repaired_and_new_files_have_valid_spdx_headers(self):
        for rel in REPAIRED_SPDX_FILES:
            path = os.path.join(ROOT, rel)
            with self.subTest(path=rel):
                with open(path, encoding="utf-8") as handle:
                    lines = handle.readlines()
                validate_spdx_header(lines, comment_prefix(rel), "2026")
                if rel == "tools/mcp/scripts/mcp.py":
                    self.assertTrue(
                        lines[0].startswith("# SPDX-FileCopyrightText:"),
                        "mcp.py must place SPDX headers before module docstring",
                    )
                    self.assertIn('"""Tiny MCP client', lines[3])

    def test_fidelityfx_sdk_headers_do_not_carry_eglantine_gpl(self):
        pattern = os.path.join(ROOT, "externals", "FidelityFX-SDK", "**", "*.h")
        headers = sorted(glob.glob(pattern, recursive=True))
        self.assertGreaterEqual(len(headers), 22)
        for path in headers:
            rel = os.path.relpath(path, ROOT)
            with self.subTest(path=rel):
                with open(path, encoding="utf-8", errors="replace") as handle:
                    head = "".join(handle.readlines()[:8])
                self.assertNotIn("Eglantine Emulator Project", head)
                self.assertNotIn("GPL-3.0-or-later", head)
                self.assertIn("FidelityFX SDK", head)


class LicenseHeaderShellExecutionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not has_working_posix_shell():
            raise unittest.SkipTest("POSIX sh/awk is unavailable in this environment")
        cls.year = str(datetime.date.today().year)

    def run_script(self, *args):
        return subprocess.run(
            ["sh", SCRIPT, *args],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=15,
        )

    def write_temp_file(self, tmpdir, name, content, executable=False):
        path = os.path.join(tmpdir, name)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(content)
        mode = stat.S_IRUSR | stat.S_IWUSR
        if executable:
            mode |= stat.S_IXUSR
        os.chmod(path, mode)
        return path

    def test_accepts_valid_multi_copyright_and_third_party_attribution_styles(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            nix_file = self.write_temp_file(
                tmpdir,
                "flake.nix",
                f"# SPDX-FileCopyrightText: Copyright {self.year} {HOLDER}\n"
                f"# SPDX-FileCopyrightText: Copyright {self.year} Eglantine Community\n"
                f"# SPDX-License-Identifier: {LICENSE}\n"
                "{\n}\n",
            )
            cpp_file = self.write_temp_file(
                tmpdir,
                "macro.h",
                f"// SPDX-FileCopyrightText: Copyright {self.year} {HOLDER}\n"
                f"// SPDX-License-Identifier: {LICENSE}\n\n"
                "// Copyright (c) 2022 Collabora Ltd. SPDX-License-Identifier: MIT\n"
                "#pragma once\n",
            )
            kt_file = self.write_temp_file(
                tmpdir,
                "Doc.kt",
                f"// SPDX-FileCopyrightText: Copyright {self.year} {HOLDER}\n"
                f"// SPDX-License-Identifier: {LICENSE}\n\n"
                "// SPDX-License-Identifier: MPL-2.0\n"
                "// Copyright 2023 Skyline Team and Contributors\n"
                "class Doc {}\n",
            )
            polyglot_files = []
            for fname, body in (
                ("lib.rs", "pub fn ok() {}\n"),
                ("kernel.slang", "[shader(\"compute\")] void main() {}\n"),
                ("vec.ispc", "export void mix_pcm() {}\n"),
                ("ui.slint", "export component MainWindow {}\n"),
            ):
                polyglot_files.append(
                    self.write_temp_file(
                        tmpdir,
                        fname,
                        f"// SPDX-FileCopyrightText: Copyright {self.year} {HOLDER}\n"
                        f"// SPDX-License-Identifier: {LICENSE}\n\n"
                        f"{body}",
                    )
                )
            res = self.run_script("--check", nix_file, cpp_file, kt_file, *polyglot_files)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

    def test_rejects_corrupted_and_missing_headers_with_nonzero_exit(self):
        cases = {
            "missing.cpp": "#pragma once\n",
            "missing.rs": "pub fn nope() {}\n",
            "missing.slang": "void main() {}\n",
            "missing.ispc": "export void f() {}\n",
            "missing.slint": "export component App {}\n",
            "orphan_nix.nix": (
                f"# SPDX-FileCopyrightText: Copyright {self.year} Eglantine Community\n\n"
                f"# SPDX-FileCopyrightText: Copyright {self.year} {HOLDER}\n"
                f"# SPDX-License-Identifier: {LICENSE}\n{{\n}}\n"
            ),
            "dup_lic.py": (
                "#!/usr/bin/env python3\n\n"
                f"# SPDX-FileCopyrightText: Copyright {self.year} {HOLDER}\n"
                f"# SPDX-License-Identifier: {LICENSE}\n"
                f"# SPDX-License-Identifier: {LICENSE}\n"
                "print('ok')\n"
            ),
            "conflict_lic.py": (
                "#!/usr/bin/env python3\n\n"
                f"# SPDX-FileCopyrightText: Copyright {self.year} {HOLDER}\n"
                f"# SPDX-License-Identifier: {LICENSE}\n"
                "# SPDX-License-Identifier: GPL-2.0-or-later\n"
                "print('ok')\n"
            ),
            "split_docstring.py": (
                '"""Module doc.\n\n'
                f"# SPDX-FileCopyrightText: Copyright {self.year} {HOLDER}\n"
                f"# SPDX-License-Identifier: {LICENSE}\n"
                '"""\n'
            ),
        }
        with tempfile.TemporaryDirectory() as tmpdir:
            for name, content in cases.items():
                with self.subTest(case=name):
                    path = self.write_temp_file(tmpdir, name, content)
                    res = self.run_script("--check", path)
                    self.assertNotEqual(res.returncode, 0, f"{name} unexpectedly passed check")

    def test_update_preserves_non_shebang_files_and_does_not_chmod(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            nix_file = self.write_temp_file(tmpdir, "default.nix", "{ pkgs }:\npkgs.hello\n")
            mod_py = self.write_temp_file(tmpdir, "mod.py", '"""Docstring line 1."""\nx = 1\n')
            cli_py = self.write_temp_file(
                tmpdir, "cli.py", "#!/usr/bin/env python3\nimport sys\n", executable=True
            )
            bare_cpp = self.write_temp_file(
                tmpdir, "bare.cpp", f"// SPDX-License-Identifier: {LICENSE}\nint f() {{ return 0; }}\n"
            )

            upd = self.run_script("--update", nix_file, mod_py, cli_py, bare_cpp)
            self.assertEqual(upd.returncode, 0, upd.stdout + upd.stderr)

            chk = self.run_script("--check", nix_file, mod_py, cli_py, bare_cpp)
            self.assertEqual(chk.returncode, 0, chk.stdout + chk.stderr)

            with open(nix_file, encoding="utf-8") as handle:
                self.assertIn("{ pkgs }:", handle.read())
            self.assertFalse(bool(os.stat(nix_file).st_mode & stat.S_IXUSR))

            with open(mod_py, encoding="utf-8") as handle:
                mod_lines = handle.read().splitlines()
            self.assertTrue(mod_lines[0].startswith("# SPDX-FileCopyrightText:"))
            self.assertIn('"""Docstring line 1."""', mod_lines)
            self.assertFalse(bool(os.stat(mod_py).st_mode & stat.S_IXUSR))

            with open(cli_py, encoding="utf-8") as handle:
                cli_lines = handle.read().splitlines()
            self.assertEqual(cli_lines[0], "#!/usr/bin/env python3")
            self.assertTrue(cli_lines[2].startswith("# SPDX-FileCopyrightText:"))

            with open(bare_cpp, encoding="utf-8") as handle:
                bare_lines = handle.read().splitlines()
            self.assertEqual(bare_lines.count(f"// SPDX-License-Identifier: {LICENSE}"), 1)


if __name__ == "__main__":
    unittest.main()
