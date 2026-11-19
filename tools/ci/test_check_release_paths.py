# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

import io
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
import warnings
import zipfile


SCANNER = Path(__file__).with_name("check_release_paths.py")


class ReleasePathTests(unittest.TestCase):
    def scan(self, *args, data=b""):
        return subprocess.run(
            [sys.executable, "-I", str(SCANNER), *args],
            input=data,
            capture_output=True,
            text=False,
            check=False,
        )

    def assert_rejected(self, result, *diagnostics):
        self.assertEqual(result.returncode, 1, result.stderr)
        for diagnostic in diagnostics:
            self.assertIn(diagnostic, result.stderr)

    def test_public_fixtures_pass_tree_scan(self):
        result = self.scan(
            "--stdin0",
            data=b"src/service/ignore/a.cpp\0fixture.bin\0logo.png\0src/Assets.car\0",
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_blocked_tree_paths_are_reported(self):
        result = self.scan(
            "--stdin0",
            data=(
                b"ignore/with space.txt\0.Lab/tool.txt\0dist/title.keys\0Game.XcI.01\0"
                b"dump/abcd0123456789ef-gfx-1234.bin\0rejected-012abc.bin\0data.NUP\0"
                b"dist/Assets.car\0"
            ),
        )
        self.assert_rejected(
            result,
            b"'ignore/with space.txt': private worktree path",
            b"'.Lab/tool.txt': private worktree path",
            b"'dist/title.keys': console key file",
            b"'Game.XcI.01': console content archive",
            b"'dump/abcd0123456789ef-gfx-1234.bin': emulator capture",
            b"'rejected-012abc.bin': emulator capture",
            b"'data.NUP': console content archive",
            b"'dist/Assets.car': quarantined asset",
        )

    def test_incomplete_tree_input_is_rejected(self):
        self.assert_rejected(self.scan("--stdin0", data=b"source.cpp"), b"missing final NUL")

    def test_archive_formats_and_members(self):
        with tempfile.TemporaryDirectory() as tmp:
            zip_path = Path(tmp) / "release.zip"
            tar_path = Path(tmp) / "release.tar.gz"
            with zipfile.ZipFile(zip_path, "w") as bundle:
                bundle.writestr("Eglantine.app/Contents/MacOS/eglantine", b"test")
                bundle.writestr("Eglantine.app/Contents/Frameworks/Qt.framework/Resources/Assets.car", b"test")
            with tarfile.open(tar_path, "w:gz") as bundle:
                entry = tarfile.TarInfo("./eglantine")
                entry.size = 4
                bundle.addfile(entry, io.BytesIO(b"test"))
            result = self.scan("--archives", str(zip_path), str(tar_path))
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_unsafe_and_private_archive_members_are_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / "release.zip"
            with zipfile.ZipFile(archive, "w") as bundle:
                bundle.writestr("../rom.nsp", b"test")
                bundle.writestr("source/ignore/report.txt", b"test")
                bundle.writestr("source/dist/Assets.car", b"test")
                bundle.writestr("Eglantine.app/Contents/Resources/Assets.car", b"test")
                bundle.writestr("native-inline-compute-shader.bin", b"test")
                bundle.writestr("eglantine", b"test")
                bundle.writestr("Eglantine", b"test")
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore", UserWarning)
                    bundle.writestr("eglantine", b"test")
            self.assert_rejected(
                self.scan("--archives", str(archive)),
                b"'../rom.nsp': unsafe path",
                b"'source/ignore/report.txt': private worktree path",
                b"'source/dist/Assets.car': quarantined asset",
                b"'Eglantine.app/Contents/Resources/Assets.car': quarantined asset",
                b"'native-inline-compute-shader.bin': emulator capture",
                b"'Eglantine': duplicate archive member",
                b"'eglantine': duplicate archive member",
            )

    def test_console_content_in_tar_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / "release.tar.gz"
            with tarfile.open(archive, "w:gz") as bundle:
                entry = tarfile.TarInfo("workspace/fixture.NSZ")
                bundle.addfile(entry)
            self.assert_rejected(
                self.scan("--archives", str(archive)),
                b"'workspace/fixture.NSZ': console content archive",
            )

    def test_corrupt_archive_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / "release.tar.gz"
            archive.write_bytes(b"broken gzip")
            self.assert_rejected(self.scan("--archives", str(archive)), b"cannot read archive")


if __name__ == "__main__":
    unittest.main()
