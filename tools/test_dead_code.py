#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

import io
import json
import subprocess
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import dead_code  # noqa: E402


class DeadCodeTests(unittest.TestCase):
    def test_parses_msvc_gnu_ld_and_lld_outputs(self):
        lines = [
            '  Discarded "void __cdecl Core::UnusedHelper(int)" (?UnusedHelper@Core@@YAXH@Z) from core.lib(helper.cpp.obj)',
            "  Discarded ?RawUnused@VideoCore@@YAXXZ from D:\\Eglantine\\build\\src\\video_core\\CMakeFiles\\video_core.dir\\renderer_vulkan\\vk_pipeline_cache.cpp.obj",
            "ld: removing unused section '.text._ZN4Core12DeadFunctionEv' in file 'src/core/CMakeFiles/core.dir/core.cpp.o'",
            "ld.lld: removing unused section src/audio_core/CMakeFiles/audio_core.dir/sink.cpp.o:(.data._ZN9AudioCore9DeadTableE)",
        ]
        entries = dead_code.parse_linker_output(lines, demangle=False)
        self.assertEqual(len(entries), 4)

        self.assertEqual(entries[0].symbol, "void __cdecl Core::UnusedHelper(int)")
        self.assertEqual(entries[0].mangled, "?UnusedHelper@Core@@YAXH@Z")
        self.assertEqual(entries[0].kind, "function")
        self.assertEqual(entries[0].source_file, "core.lib(helper.cpp.obj)")

        self.assertEqual(entries[1].symbol, "?RawUnused@VideoCore@@YAXXZ")
        self.assertEqual(entries[2].symbol, "_ZN4Core12DeadFunctionEv")
        self.assertEqual(entries[2].kind, "function")
        self.assertEqual(entries[3].symbol, "_ZN9AudioCore9DeadTableE")
        self.assertEqual(entries[3].kind, "data")

    def test_first_party_and_third_party_filtering(self):
        lines = [
            '  Discarded "void __cdecl Core::Unused()" (?Unused@Core@@YAXXZ) from core.lib(core.cpp.obj)',
            '  Discarded "void __cdecl VideoCore::UnusedVk()" (?UnusedVk@VideoCore@@YAXXZ) from build/src/video_core/CMakeFiles/video_core.dir/renderer_vulkan/vk_pipeline_cache.cpp.obj',
            '  Discarded "void __cdecl fmt::v12::unused()" (?unused@v12@fmt@@YAXXZ) from fmt.lib(format.cc.obj)',
            "ld: removing unused section '.text.boost_unused' in file 'build/_deps/boost-build/libs/system/error_code.cpp.o'",
            "ld: removing unused section '.text.ext_unused' in file 'externals/glad/src/gl.c.o'",
            '  Discarded "void __cdecl crt_unused()" (?crt_unused@@YAXXZ) from vcruntime.lib(init.obj)',
        ]
        entries = dead_code.parse_linker_output(lines, demangle=False)
        first_party = dead_code.filter_entries(entries, first_party_only=True, exclude_noise=False)
        self.assertEqual(
            [e.source_file for e in first_party],
            [
                "core.lib(core.cpp.obj)",
                "build/src/video_core/CMakeFiles/video_core.dir/renderer_vulkan/vk_pipeline_cache.cpp.obj",
            ],
        )

        all_sources = dead_code.filter_entries(entries, first_party_only=False, exclude_noise=False)
        self.assertEqual(len(all_sources), 6)

    def test_noise_filtering_and_include_noise_toggle(self):
        lines = [
            '  Discarded "const Core::Foo::`vftable\'" (??_7Foo@Core@@6B@) from core.lib(foo.cpp.obj)',
            '  Discarded "public: virtual void * __cdecl Core::Foo::`scalar deleting destructor\'(unsigned int)" (??_GFoo@Core@@UEAAPEAXI@Z) from core.lib(foo.cpp.obj)',
            '  Discarded "??_C@_05ABCD@hello?$AA@" (??_C@_05ABCD@hello?$AA@) from core.lib(foo.cpp.obj)',
            '  Discarded "class std::vector<int> __cdecl Core::MakeVec()" (?MakeVec@Core@@YA?AV?$vector@H@std@@XZ) from core.lib(foo.cpp.obj)',
            '  Discarded "void __cdecl Java_org_yuzu_Native_init()" (Java_org_yuzu_Native_init) from core.lib(jni.cpp.obj)',
            '  Discarded "enum SDL_AppResult __cdecl SDL_AppInit(void **,int,char **)" (SDL_AppInit) from yuzu_cmd.dir/yuzu.cpp.obj',
            '  Discarded "void __cdecl Core::ShutdownMainProcessOrphan()" (?ShutdownMainProcessOrphan@Core@@YAXXZ) from core.lib(core.cpp.obj)',
            '  Discarded "int __cdecl main(int,char **)" (main) from yuzu_cmd.dir/yuzu.cpp.obj',
            '  Discarded $unwind$?DeadFunc@Core@@YAXXZ from core.lib(core.cpp.obj)',
            '  Discarded $pdata$?DeadFunc@Core@@YAXXZ from core.lib(core.cpp.obj)',
            '  Discarded $cppxdata$??0Foo@Core@@QEAA@XZ from core.lib(core.cpp.obj)',
            '  Discarded "struct guid::guid const winrt::impl::guid_v<int>" (?guid_v@impl@winrt@@3Uguid@2@B) from input_common.lib(switch2_ble_win.cpp.obj)',
        ]
        entries = dead_code.parse_linker_output(lines, demangle=False)
        clean = dead_code.filter_entries(entries, first_party_only=True, exclude_noise=True)
        self.assertEqual([e.symbol for e in clean], ["void __cdecl Core::ShutdownMainProcessOrphan()"])

        with_noise = dead_code.filter_entries(entries, first_party_only=True, exclude_noise=False)
        self.assertEqual(len(with_noise), 12)

    def test_deduplication_filter_json_and_fail_on_findings(self):
        sample_log = "\n".join([
            '  Discarded "void __cdecl Core::DeadAlpha()" (?DeadAlpha@Core@@YAXXZ) from core.lib(a.cpp.obj)',
            '  Discarded "void __cdecl Core::DeadAlpha()" (?DeadAlpha@Core@@YAXXZ) from core.lib(a.cpp.obj)',
            '  Discarded "void __cdecl Core::DeadBeta()" (?DeadBeta@Core@@YAXXZ) from core.lib(b.cpp.obj)',
        ])

        script = str(HERE / "dead_code.py")
        res = subprocess.run(
            [sys.executable, "-I", script, "parse", "--json", "--filter", "alpha", "--fail-on-findings"],
            input=sample_log,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(res.returncode, 1)
        payload = json.loads(res.stdout)
        self.assertEqual(len(payload), 1)
        self.assertEqual(payload[0]["symbol"], "void __cdecl Core::DeadAlpha()")

        clean_res = subprocess.run(
            [sys.executable, "-I", script, "parse", "--fail-on-findings"],
            input='  Discarded "void __cdecl fmt::dead()" (?dead@fmt@@YAXXZ) from fmt.lib(f.obj)\n',
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(clean_res.returncode, 0)
        self.assertIn("No dead first-party symbols found", clean_res.stdout)

    def test_demangle_itanium_invokes_cxxfilt_when_present(self):
        lines = ["ld: removing unused section '.text._ZN4Core6OrphanEv' in file 'src/core/core.cpp.o'"]
        fake_proc = mock.Mock(returncode=0, stdout="Core::Orphan()\n")
        with mock.patch("dead_code.find_demangler", return_value="c++filt"), \
             mock.patch("dead_code.subprocess.run", return_value=fake_proc) as run_mock:
            entries = dead_code.parse_linker_output(lines, demangle=True)
        run_mock.assert_called_once()
        self.assertEqual(entries[0].symbol, "Core::Orphan()")
        self.assertEqual(entries[0].mangled, "_ZN4Core6OrphanEv")
        self.assertEqual(entries[0].kind, "function")


if __name__ == "__main__":
    unittest.main()
