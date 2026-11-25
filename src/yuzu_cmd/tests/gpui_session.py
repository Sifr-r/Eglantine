# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

"""Cross-platform smoke and protocol tests for GPUI's private JSONL subprocess modes."""

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


def build_cli_env(cli_source):
    env = dict(os.environ)
    parent = str(cli_source.parent)
    if sys.platform == "win32":
        env["PATH"] = parent + os.pathsep + env.get("PATH", "")
    else:
        env["LD_LIBRARY_PATH"] = parent + os.pathsep + env.get("LD_LIBRARY_PATH", "")
        if sys.platform == "darwin":
            env["DYLD_LIBRARY_PATH"] = parent + os.pathsep + env.get("DYLD_LIBRARY_PATH", "")
    env["EDEN_NX2_HIDE_WINDOW"] = "1"
    return env


class SessionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="eglantine-gpui-session-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        cls.cli = cls.root / CLI.name
        shutil.copy2(CLI, cls.cli)
        (cls.root / "user").mkdir()
        cls.library = cls.root / "Biblïothèque"
        cls.library.mkdir()
        cls.rom = cls.library / "Églantine.nro"
        write_homebrew(cls.rom)
        cls.env = build_cli_env(CLI)

    def run_cli(self, *args, env=None, cwd=None):
        result = subprocess.run(
            [self.cli, *args], input="", capture_output=True, encoding="utf-8",
            env=self.env if env is None else env, cwd=self.root if cwd is None else cwd,
            timeout=30, check=False,
        )
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertTrue(all(row["version"] == 1 for row in rows))
        return result, rows

    def start_session(self):
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

        return process, receive

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

    def test_library_scan_depth_and_rejects_invalid_containers(self):
        directory = self.root / "library discovery"
        nested = directory / "nested"
        nested.mkdir(parents=True)
        root_game = directory / "root.nro"
        nested_game = nested / "nested.nro"
        write_homebrew(root_game)
        write_homebrew(nested_game)
        (directory / "not-a-game.nup").write_bytes(b"invalid container")

        for options, expected in (
            (("--shallow",), {root_game.resolve()}),
            ((), {root_game.resolve(), nested_game.resolve()}),
        ):
            result, rows = self.run_cli("--gpui-library", directory, *options)
            self.assertEqual(result.returncode, 0, (rows, result.stderr))
            self.assertEqual(rows[-1]["event"], "library_done")
            self.assertEqual(rows[-1]["scan_errors"], 0)
            games = [Path(row["path"]) for row in rows if row["event"] == "game"]
            self.assertCountEqual(games, expected)

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
        process, receive = self.start_session()

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

    def test_unknown_command_and_malformed_json_keep_session_running(self):
        process, receive = self.start_session()
        receive("ready")
        self.assertEqual(receive("state")["state"], "running")

        process.stdin.write("{not json\n")
        process.stdin.flush()
        self.assertIn("Invalid", receive("error")["message"])

        process.stdin.write('{"version":1,"command":"pause"}\n')
        process.stdin.flush()
        self.assertIn("Invalid", receive("error")["message"])

        process.stdin.write(json.dumps({"version": 1, "id": 10, "command": "bogus"}) + "\n")
        process.stdin.flush()
        reply = receive("reply")
        self.assertEqual(reply["id"], 10)
        self.assertFalse(reply["ok"])
        self.assertEqual(reply["state"], "running")

        process.stdin.write(json.dumps({"version": 1, "id": 11, "command": "stop"}) + "\n")
        process.stdin.flush()
        self.assertEqual(receive("state")["state"], "stopping")
        stop_reply = receive("reply")
        self.assertTrue(stop_reply["ok"])
        self.assertEqual(stop_reply["id"], 11)
        self.assertEqual(receive("state")["state"], "stopped")
        self.assertEqual(process.wait(timeout=30), 0)

    def test_oversized_request_emits_error_and_exits(self):
        process, receive = self.start_session()
        receive("ready")
        self.assertEqual(receive("state")["state"], "running")

        process.stdin.write("x" * 4100 + "\n")
        process.stdin.flush()
        self.assertIn("size limit", receive("error")["message"])
        self.assertEqual(receive("state")["state"], "stopping")
        self.assertEqual(receive("state")["state"], "stopped")
        self.assertEqual(process.wait(timeout=30), 0)

    def test_closing_stdin_triggers_orderly_shutdown(self):
        process, receive = self.start_session()
        receive("ready")
        self.assertEqual(receive("state")["state"], "running")

        process.stdin.close()
        self.assertEqual(receive("state")["state"], "stopping")
        self.assertEqual(receive("state")["state"], "stopped")
        self.assertEqual(process.wait(timeout=30), 0)

    def test_gpui_content_validation_and_empty_verify(self):
        with tempfile.TemporaryDirectory(prefix="eglantine-gpui-content-") as tmp:
            work = Path(tmp)

            result, rows = self.run_cli("--gpui-content")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual([row["event"] for row in rows], ["ready", "error"])

            result, rows = self.run_cli("--gpui-content", "unknown_op")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual([row["event"] for row in rows], ["ready", "error"])

            result, rows = self.run_cli("--gpui-content", "verify_content", str(work))
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual([row["event"] for row in rows], ["ready", "error"])

            result, rows = self.run_cli("--gpui-content", "verify_content")
            self.assertEqual(result.returncode, 0, (rows, result.stderr))
            self.assertEqual([row["event"] for row in rows], ["ready", "content_done"])

            txt_file = work / "notes.txt"
            txt_file.write_text("not content", encoding="utf-8")
            nca_file = work / "0123456789abcdef0123456789abcdef.nca"
            nca_file.write_bytes(b"\x00" * 16)
            nsp_file = work / "update.nsp"
            nsp_file.write_bytes(b"\x00" * 16)

            for args in (
                ("--gpui-content", "install_content", str(work / "missing.nsp")),
                ("--gpui-content", "install_content", str(txt_file)),
                ("--gpui-content", "install_content", str(nca_file)),
                ("--gpui-content", "install_nca_update", str(nsp_file)),
            ):
                result, rows = self.run_cli(*args)
                self.assertNotEqual(result.returncode, 0, args)
                self.assertEqual([row["event"] for row in rows], ["ready", "error"], args)

            empty_fw = work / "empty_fw"
            empty_fw.mkdir()
            result, rows = self.run_cli("--gpui-content", "install_firmware", str(empty_fw))
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual([row["event"] for row in rows], ["ready", "error"])

            bad_fw = work / "bad_fw"
            bad_fw.mkdir()
            (bad_fw / "bad_name.nca").write_bytes(b"\x00" * 16)
            result, rows = self.run_cli("--gpui-content", "install_firmware", str(bad_fw))
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual([row["event"] for row in rows], ["ready", "error"])
            self.assertIn("invalid NCA", rows[-1]["message"])

            registered = self.root / "user" / "nand" / "system" / "Contents" / "registered"
            registered.mkdir(parents=True, exist_ok=True)
            marker = registered / "existing.nca"
            marker.write_bytes(b"\x00" * 16)
            try:
                result, rows = self.run_cli("--gpui-content", "install_firmware", str(empty_fw))
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual([row["event"] for row in rows], ["ready", "error"])
                self.assertIn("empty system NAND", rows[-1]["message"])
            finally:
                marker.unlink(missing_ok=True)

    @staticmethod
    def reap(process):
        if process.poll() is None:
            process.kill()
            process.wait(timeout=15)
        if process.stdin and not process.stdin.closed:
            process.stdin.close()
        if process.stdout and not process.stdout.closed:
            process.stdout.close()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: python -I gpui_session.py path/to/eden-cli")
    CLI = Path(sys.argv[1]).resolve(strict=True)
    unittest.main(argv=sys.argv[:1])
