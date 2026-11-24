# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

"""Windows smoke test for GPUI's private JSONL subprocess modes."""

import json
import os
from pathlib import Path
import queue
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import unittest


def write_homebrew(path):
    """Three pages of synthetic NRO that yields through SleepThread."""
    image = bytearray(0x3000)
    struct.pack_into("<I", image, 0, 0x14000020)  # b +0x80
    image[0x08:0x14] = b"HOMEBREWNRO0"
    struct.pack_into("<I", image, 0x18, len(image))
    struct.pack_into("<6I", image, 0x20, 0, 0x1000, 0x1000, 0x1000, 0x2000, 0x1000)
    struct.pack_into("<3I", image, 0x80, 0xD2A1E840, 0xD4000161, 0x17FFFFFE)
    path.write_bytes(image)


class SessionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="eglantine-gpui-session-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        cls.cli = cls.root / CLI.name
        shutil.copyfile(CLI, cls.cli)
        (cls.root / "user").mkdir()
        cls.library = cls.root / "Biblïothèque"
        cls.library.mkdir()
        cls.rom = cls.library / "Églantine.nro"
        write_homebrew(cls.rom)
        cls.env = dict(os.environ)
        cls.env["PATH"] = str(CLI.parent) + os.pathsep + cls.env.get("PATH", "")
        cls.env["EDEN_NX2_HIDE_WINDOW"] = "1"

    def run_cli(self, *args, env=None):
        result = subprocess.run(
            [self.cli, *args], input="", capture_output=True, encoding="utf-8",
            env=self.env if env is None else env, cwd=self.root, timeout=30, check=False,
        )
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertTrue(all(row["version"] == 1 for row in rows))
        return result, rows

    def test_missing_game_reports_error(self):
        result, rows = self.run_cli("--gpui-session")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual([row["event"] for row in rows], ["ready", "error", "state", "state"])
        self.assertEqual([row["state"] for row in rows[2:]], ["stopping", "stopped"])

    def test_library_canonical_path_and_metadata(self):
        result, rows = self.run_cli("--gpui-library", self.library, "--shallow")
        self.assertEqual(result.returncode, 0, (rows, result.stderr))
        self.assertEqual([row["event"] for row in rows], ["ready", "game", "library_done"])
        self.assertEqual(Path(rows[1]["path"]), self.rom.resolve())
        self.assertEqual(rows[1]["title"], self.rom.stem)
        self.assertEqual(rows[1]["format"], "NRO")

    def test_library_config_is_read_only(self):
        config = self.root / "préférences.ini"
        original = b"; Read-only settings fixture\n[Controls]\n"
        config.write_bytes(original)
        environment = dict(self.env, EGLANTINE_GPUI_CONFIG=str(config))
        result, rows = self.run_cli("--gpui-library", self.library, env=environment)
        self.assertEqual(result.returncode, 0, (rows, result.stderr))
        self.assertEqual(config.read_bytes(), original)

        missing = config.with_suffix(".missing")
        environment["EGLANTINE_GPUI_CONFIG"] = str(missing)
        result, rows = self.run_cli("--gpui-library", self.library, env=environment)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(rows[-1]["event"], "error")
        self.assertFalse(missing.exists())

    def test_pause_resume_stop_and_framing(self):
        diagnostics = self.root / "session.stderr"
        stderr = diagnostics.open("wb")
        self.addCleanup(stderr.close)
        process = subprocess.Popen(
            [self.cli, "--gpui-session", "-g", self.rom, "--null-render"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
            encoding="utf-8", env=self.env, cwd=self.root,
        )
        self.addCleanup(self.reap, process)
        messages = queue.Queue()

        def read_messages():
            for line in process.stdout:
                messages.put(json.loads(line))
            messages.put(None)

        threading.Thread(target=read_messages, daemon=True).start()

        def receive(event):
            try:
                row = messages.get(timeout=30)
            except queue.Empty:
                self.fail(f"Timed out waiting for {event}: {diagnostics.read_text(errors='replace')[-4000:]}")
            self.assertIsNotNone(row, "the gameplay host exited early")
            self.assertEqual(row["version"], 1)
            self.assertEqual(row["event"], event, row)
            return row

        def send(command, request_id, version=1):
            value = json.dumps({"version": version, "id": request_id, "command": command})
            process.stdin.write(value[:9])
            process.stdin.flush()
            process.stdin.write(value[9:] + "\n")
            process.stdin.flush()

        receive("ready")
        self.assertEqual(receive("state")["state"], "running")
        send("pause", 1, version=2)
        self.assertFalse(receive("reply")["ok"])
        for command, state, request_id in (
            ("pause", "paused", 2), ("resume", "running", 4), ("stop", "stopping", 5),
        ):
            send(command, request_id)
            self.assertEqual(receive("state")["state"], state)
            self.assertEqual(receive("reply"), {
                "version": 1, "event": "reply", "id": request_id, "ok": True, "state": state,
            })
            if command == "pause":
                send(command, 3)
                self.assertEqual(receive("reply")["state"], "paused")
        self.assertEqual(receive("state")["state"], "stopped")
        self.assertEqual(process.wait(timeout=30), 0)

    @staticmethod
    def reap(process):
        if process.poll() is None:
            process.kill()
            process.wait(timeout=15)
        process.stdin.close()
        process.stdout.close()


if __name__ == "__main__":
    if sys.platform != "win32" or len(sys.argv) != 2:
        sys.exit("Usage on Windows: python -I gpui_session.py path/to/eden-cli.exe")
    CLI = Path(sys.argv[1]).resolve(strict=True)
    unittest.main(argv=sys.argv[:1])
