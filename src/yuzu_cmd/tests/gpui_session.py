# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

"""Cross-platform smoke and protocol tests for GPUI's private JSONL subprocess modes."""

import hashlib
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
import time
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


def game_boy_header(color_flag=0):
    image = bytearray(0x8000)
    image[0x104:0x108] = bytes.fromhex("CE ED 66 66")
    image[0x134:0x13B] = b"EXAMPLE"
    image[0x143] = color_flag
    image[0x14D] = (-sum(image[0x134:0x14D]) - 0x19) & 0xFF
    return image


def game_boy_advance_header():
    image = bytearray(0x8000)
    image[0x04:0x08] = bytes.fromhex("24 FF AE 51")
    image[0xA0:0xA7] = b"EXAMPLE"
    image[0xB2] = 0x96
    image[0xBD] = (-sum(image[0xA0:0xBD]) - 0x19) & 0xFF
    return image


def write_loose_exefs(title_folder, switch_2):
    exefs = title_folder / "exefs"
    exefs.mkdir(parents=True)
    (exefs / "main").write_bytes(b"NSO0" + bytes(0xFC))
    aci_offset = 0x80
    acid_offset = 0x100
    acid_body = 0x300 if switch_2 else 0x200
    acid_size = acid_body + 0x40 + 0x2C
    npdm = bytearray(acid_offset + acid_size)
    npdm[:4] = b"META"
    struct.pack_into("<4I", npdm, 0x70, aci_offset, 0x80, acid_offset, acid_size)
    npdm[aci_offset:aci_offset + 4] = b"ACI0"
    struct.pack_into("<Q", npdm, aci_offset + 0x10, 0x0100000000001000)
    struct.pack_into("<2I", npdm, aci_offset + 0x20, 0x40, 0x1C)
    acid_magic_offset = acid_offset + acid_body
    npdm[acid_magic_offset:acid_magic_offset + 4] = b"ACID"
    struct.pack_into("<2I", npdm, acid_magic_offset + 0x20, acid_body + 0x40, 0x2C)
    (exefs / "main.npdm").write_bytes(npdm)
    return exefs / "main"


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

    def start_session(self, rom=None, env=None):
        diagnostics = self.root / "session.stderr"
        stderr = diagnostics.open("wb")
        self.addCleanup(stderr.close)
        process = subprocess.Popen(
            [self.cli, "--gpui-session", "-g", rom or self.rom, "--null-render"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
            encoding="utf-8", env=env or self.env, cwd=self.root,
        )
        self.addCleanup(self.reap, process)
        messages = queue.Queue()

        def read_messages():
            for line in process.stdout:
                messages.put(json.loads(line))
            messages.put(None)

        threading.Thread(target=read_messages, daemon=True).start()

        def receive(event=None):
            try:
                row = messages.get(timeout=30)
            except queue.Empty:
                self.fail(f"Timed out waiting for {event}: {diagnostics.read_text(errors='replace')[-4000:]}")
            self.assertIsNotNone(row, "the gameplay host exited early")
            self.assertEqual(row["version"], 1)
            if event is not None:
                self.assertEqual(row["event"], event, row)
            return row

        return process, receive

    def test_classic_lifecycle_and_battery_save(self):
        env = dict(self.env, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                   SDL_RENDER_DRIVER="software")
        game_boy = game_boy_header()
        game_boy[0x100:0x104] = bytes.fromhex("00 C3 50 01")
        game_boy[0x147] = 0x03  # MBC1 with battery-backed RAM.
        game_boy[0x149] = 0x02
        game_boy[0x14D] = (-sum(game_boy[0x134:0x14D]) - 0x19) & 0xFF
        # Enable external RAM, write a byte at A000, and loop.
        game_boy[0x150:0x15C] = bytes.fromhex("3E 0A EA 00 00 3E 5A EA 00 A0 18 FE")
        game_boy_color = game_boy_header(0x80)
        game_boy_color[0x100:0x104] = bytes.fromhex("00 C3 50 01")
        game_boy_color[0x150:0x152] = bytes.fromhex("18 FE")
        advance = game_boy_advance_header()
        struct.pack_into("<I", advance, 0, 0xEA00002E)
        struct.pack_into("<I", advance, 0xC0, 0xEAFFFFFE)

        for extension, image in (("gb", game_boy), ("gbc", game_boy_color), ("gba", advance)):
            with self.subTest(extension=extension):
                rom = self.root / f"Églantine Classic.{extension}"
                rom.write_bytes(image)
                process, receive = self.start_session(rom, env)
                receive("ready")
                first = receive()
                if first["event"] == "error" and "disabled in this build" in first["message"]:
                    self.skipTest("The host was built without the optional classic runtime")
                self.assertEqual((first["event"], first.get("state")), ("state", "running"), first)

                if extension == "gb":
                    digest = hashlib.sha256(image).hexdigest()
                    save = self.root / "user" / "classic" / "saves" / "0" / "game-boy" / f"{digest}.sav"
                    deadline = time.monotonic() + 10
                    while not save.exists() and time.monotonic() < deadline:
                        time.sleep(0.05)
                    self.assertTrue(save.exists(), "The running cartridge did not publish a battery save")
                    self.assertEqual(save.read_bytes()[0], 0x5A)

                if extension == "gba":
                    for cmd_payload, req_id in (
                        ({"command": "set_video_options", "color_correction": True, "interframe_blending": False}, 10),
                        ({"command": "set_fast_forward", "enabled": True}, 11),
                        ({"command": "save_state", "slot": 1}, 12),
                        ({"command": "load_state", "slot": 1}, 13),
                        ({"command": "set_cheats", "codes": ["02000010:BEEF"]}, 14),
                        ({"command": "reset"}, 15),
                    ):
                        msg = {"version": 1, "id": req_id, **cmd_payload}
                        process.stdin.write(json.dumps(msg) + "\n")
                        process.stdin.flush()
                        reply = receive("reply")
                        self.assertEqual(reply["id"], req_id, reply)
                        self.assertTrue(reply["ok"], reply)

                for request, state, request_id in (("pause", "paused", 1), ("resume", "running", 2),
                                                   ("stop", "stopping", 3)):
                    process.stdin.write(json.dumps({"version": 1, "id": request_id, "command": request}) + "\n")
                    process.stdin.flush()
                    self.assertEqual(receive("state")["state"], state)
                    self.assertEqual(receive("reply"), {"version": 1, "event": "reply", "id": request_id,
                                                        "ok": True, "state": state})
                self.assertEqual(receive("state")["state"], "stopped")
                self.assertEqual(process.wait(timeout=30), 0)
                self.assertEqual(rom.read_bytes(), image)

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
        self.assertNotIn("system", rows[1])

    def test_library_recognizes_classic_cartridge_headers(self):
        directory = self.root / "classic cartridges"
        directory.mkdir()
        (directory / "Monochrome.gbc").write_bytes(game_boy_header())
        (directory / "Dual mode.gb").write_bytes(game_boy_header(0x80))
        (directory / "Color only.gbc").write_bytes(game_boy_header(0xC0))
        advance = game_boy_advance_header()
        advance[0xAC:0xB0] = b"AXPE"
        advance[0xB0:0xB2] = b"01"
        advance[0xBD] = (-sum(advance[0xA0:0xBD]) - 0x19) & 0xFF
        (directory / "Advance.GBA").write_bytes(advance)
        bad_checksum = game_boy_header()
        bad_checksum[0x14D] ^= 1
        (directory / "Wrong checksum.gb").write_bytes(bad_checksum)
        bad_advance_checksum = game_boy_advance_header()
        bad_advance_checksum[0xBD] ^= 1
        (directory / "Wrong advance checksum.gba").write_bytes(bad_advance_checksum)
        wrong_format = game_boy_advance_header()
        (directory / "Misnamed.gbc").write_bytes(wrong_format)
        (directory / "Short.gba").write_bytes(wrong_format[:0xBD])
        (directory / "Short.gb").write_bytes(game_boy_header()[:0x150])

        result, rows = self.run_cli("--gpui-library", directory, "--shallow")
        self.assertEqual(result.returncode, 0, (rows, result.stderr))
        games = {row["title"]: row for row in rows if row["event"] == "game"}
        matches = {title: (row["format"], row["system"]) for title, row in games.items()}
        self.assertEqual(matches, {
            "Monochrome": ("GB", "game-boy"),
            "Dual mode": ("GBC", "game-boy-color"),
            "Color only": ("GBC", "game-boy-color"),
            "Advance": ("GBA", "game-boy-advance"),
        })
        self.assertEqual(games["Monochrome"]["internal_title"], "EXAMPLE")
        self.assertEqual(games["Monochrome"]["hardware"], "ROM Only")
        self.assertEqual(games["Advance"]["internal_title"], "EXAMPLE")
        self.assertEqual(games["Advance"]["product_code"], "AXPE")
        self.assertEqual(games["Advance"]["region"], "USA")
        self.assertEqual(games["Advance"]["publisher"], "Nintendo")
        self.assertEqual(games["Advance"]["file_size"], len(advance))
        self.assertEqual(rows[-1]["scan_errors"], 0)

    def test_library_classifies_loose_switch_program_metadata(self):
        directory = self.root / "switch systems"
        switch = write_loose_exefs(directory / "Switch example", switch_2=False)
        switch_2 = write_loose_exefs(directory / "Switch 2 example", switch_2=True)
        incomplete = write_loose_exefs(directory / "Incomplete example", switch_2=True)
        npdm_path = incomplete.with_suffix(".npdm")
        npdm = bytearray(npdm_path.read_bytes())
        struct.pack_into("<2I", npdm, 0xB0, 0x60, 0x1000)
        npdm_path.write_bytes(npdm)

        result, rows = self.run_cli("--gpui-library", directory, "--shallow")
        self.assertEqual(result.returncode, 0, (rows, result.stderr))
        self.assertEqual({Path(row["path"]): row.get("system") for row in rows
                          if row["event"] == "game"}, {
            switch.resolve(): "switch",
            switch_2.resolve(): "switch-2",
            incomplete.resolve(): None,
        })

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

        process.stdin.write(
            json.dumps({
                "version": 1,
                "id": 105,
                "command": "set_video_options",
                "color_correction": "invalid",
                "interframe_blending": False,
            }) + "\n"
        )
        process.stdin.flush()
        reply = receive("reply")
        self.assertEqual(reply["id"], 105)
        self.assertFalse(reply["ok"])
        self.assertIn("color_correction", reply["message"])
        self.assertEqual(reply["state"], "running")

        for payload, expected_ok in (
            ({"id": 106, "command": "set_viewport", "x": 0, "y": 48, "width": 1280, "height": 672}, True),
            ({"id": 107, "command": "set_viewport", "x": 0, "y": 48, "width": 0, "height": 672}, False),
        ):
            process.stdin.write(json.dumps({"version": 1, **payload}) + "\n")
            process.stdin.flush()
            reply = receive("reply")
            self.assertEqual(reply["id"], payload["id"])
            self.assertEqual(reply["ok"], expected_ok, reply)
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

    def start_swkbd_test(self, scenario):
        diagnostics = self.root / f"swkbd-{scenario}.stderr"
        stderr = diagnostics.open("wb")
        self.addCleanup(stderr.close)
        process = subprocess.Popen(
            [self.cli, "--gpui-swkbd-test", scenario],
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

        def receive(event=None):
            try:
                row = messages.get(timeout=20)
            except queue.Empty:
                self.fail(f"Timed out waiting for {event}: {diagnostics.read_text(errors='replace')[-4000:]}")
            self.assertIsNotNone(row, "swkbd test host exited early")
            self.assertEqual(row["version"], 1)
            if event is not None:
                self.assertEqual(row["event"], event, row)
            return row

        return process, receive

    def test_swkbd_submit_non_ascii_and_initial_text(self):
        process, receive = self.start_swkbd_test("submit")
        receive("ready")
        self.assertEqual(receive("state")["state"], "running")
        show = receive("swkbd_show")
        self.assertEqual(show["header_text"], "Enter Name")
        self.assertEqual(show["initial_text"], "Églantine")
        self.assertEqual((show["min_length"], show["max_length"]), (2, 16))
        req_id = show["request_id"]

        process.stdin.write(json.dumps({
            "version": 1, "id": 1, "command": "swkbd_submit",
            "request_id": req_id, "text": "Églantine 日本語", "confirmed": False,
        }) + "\n")
        process.stdin.flush()
        reply = receive("reply")
        self.assertTrue(reply["ok"], reply)
        self.assertEqual(receive("swkbd_close")["request_id"], req_id)
        done = receive()
        self.assertEqual(done["event"], "swkbd_test_done:1:0:ok:Églantine 日本語")
        self.assertEqual(receive("state")["state"], "stopped")
        self.assertEqual(process.wait(timeout=20), 0)

    def test_swkbd_cancel_and_disable_cancel(self):
        process, receive = self.start_swkbd_test("cancel")
        receive("ready")
        self.assertEqual(receive("state")["state"], "running")
        req_id = receive("swkbd_show")["request_id"]

        process.stdin.write(json.dumps({
            "version": 1, "id": 1, "command": "swkbd_cancel", "request_id": req_id,
        }) + "\n")
        process.stdin.flush()
        self.assertTrue(receive("reply")["ok"])
        self.assertEqual(receive("swkbd_close")["request_id"], req_id)
        self.assertEqual(receive()["event"], "swkbd_test_done:1:0:cancel:")
        self.assertEqual(receive("state")["state"], "stopped")
        self.assertEqual(process.wait(timeout=20), 0)

        process, receive = self.start_swkbd_test("disable_cancel")
        receive("ready")
        self.assertEqual(receive("state")["state"], "running")
        show = receive("swkbd_show")
        self.assertTrue(show["disable_cancel"])
        req_id = show["request_id"]

        process.stdin.write(json.dumps({
            "version": 1, "id": 1, "command": "swkbd_cancel", "request_id": req_id,
        }) + "\n")
        process.stdin.flush()
        reply = receive("reply")
        self.assertFalse(reply["ok"])
        self.assertIn("disabled", reply["message"])

        process.stdin.write(json.dumps({
            "version": 1, "id": 2, "command": "swkbd_submit",
            "request_id": req_id, "text": "Valid", "confirmed": False,
        }) + "\n")
        process.stdin.flush()
        self.assertTrue(receive("reply")["ok"])
        self.assertEqual(receive("swkbd_close")["request_id"], req_id)
        self.assertEqual(receive()["event"], "swkbd_test_done:1:0:ok:Valid")
        self.assertEqual(receive("state")["state"], "stopped")
        self.assertEqual(process.wait(timeout=20), 0)

    def test_swkbd_bounds_and_text_check_retry(self):
        process, receive = self.start_swkbd_test("text_check")
        receive("ready")
        self.assertEqual(receive("state")["state"], "running")
        show = receive("swkbd_show")
        req_id = show["request_id"]
        self.assertEqual((show["min_length"], show["max_length"]), (3, 8))

        for bad_id, payload in (
            (1, {"request_id": req_id + 99, "text": "good"}),
            (2, {"request_id": req_id, "text": "ab"}),
            (3, {"request_id": req_id, "text": "123456789"}),
        ):
            process.stdin.write(json.dumps({"version": 1, "id": bad_id, "command": "swkbd_submit", **payload}) + "\n")
            process.stdin.flush()
            self.assertFalse(receive("reply")["ok"], payload)

        # Guest rejects "bad" with Failure -> user retries.
        process.stdin.write(json.dumps({
            "version": 1, "id": 4, "command": "swkbd_submit",
            "request_id": req_id, "text": "bad", "confirmed": False,
        }) + "\n")
        process.stdin.flush()
        self.assertTrue(receive("reply")["ok"])
        check = receive("swkbd_text_check")
        self.assertEqual((check["request_id"], check["result"], check["message"]),
                         (req_id, "failure", "Name reserved"))

        # Guest asks for Confirm on "warn" -> user confirms.
        process.stdin.write(json.dumps({
            "version": 1, "id": 5, "command": "swkbd_submit",
            "request_id": req_id, "text": "warn", "confirmed": False,
        }) + "\n")
        process.stdin.flush()
        self.assertTrue(receive("reply")["ok"])
        check = receive("swkbd_text_check")
        self.assertEqual((check["request_id"], check["result"], check["message"]),
                         (req_id, "confirm", "Use warn?"))

        process.stdin.write(json.dumps({
            "version": 1, "id": 6, "command": "swkbd_submit",
            "request_id": req_id, "text": "warn", "confirmed": True,
        }) + "\n")
        process.stdin.flush()
        self.assertTrue(receive("reply")["ok"])
        self.assertEqual(receive("swkbd_close")["request_id"], req_id)
        self.assertEqual(receive()["event"], "swkbd_test_done:1:2:ok:warn")
        self.assertEqual(receive("state")["state"], "stopped")
        self.assertEqual(process.wait(timeout=20), 0)

    def test_swkbd_close_while_open_completes_once(self):
        # Closing stdin while keyboard is open cancels exactly once.
        process, receive = self.start_swkbd_test("submit")
        receive("ready")
        self.assertEqual(receive("state")["state"], "running")
        req_id = receive("swkbd_show")["request_id"]
        process.stdin.close()
        self.assertEqual(receive("swkbd_close")["request_id"], req_id)
        self.assertEqual(receive()["event"], "swkbd_test_done:1:0:cancel:")
        self.assertEqual(receive("state")["state"], "stopped")
        self.assertEqual(process.wait(timeout=20), 0)

        # Stop command while keyboard is open cancels exactly once.
        process, receive = self.start_swkbd_test("submit")
        receive("ready")
        self.assertEqual(receive("state")["state"], "running")
        req_id = receive("swkbd_show")["request_id"]
        process.stdin.write(json.dumps({"version": 1, "id": 1, "command": "stop"}) + "\n")
        process.stdin.flush()
        self.assertEqual(receive("state")["state"], "stopping")
        self.assertTrue(receive("reply")["ok"])
        self.assertEqual(receive("swkbd_close")["request_id"], req_id)
        self.assertEqual(receive()["event"], "swkbd_test_done:1:0:cancel:")
        self.assertEqual(receive("state")["state"], "stopped")
        self.assertEqual(process.wait(timeout=20), 0)

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
