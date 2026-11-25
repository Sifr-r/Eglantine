#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""C++ dead-code ("vulture") reporter for Eglantine.

Parses linker GC reports from MSVC (/OPT:REF /VERBOSE:REF), GNU ld (--print-gc-sections),
and LLD (--print-gc-sections), filters out third-party libraries and C++ ABI/template noise,
and optionally invokes clang-tidy / cppcheck for intra-translation-unit unused checks.
"""

import argparse
from collections import defaultdict
from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

MSVC_QUOTED_RE = re.compile(
    r'^\s*Discarded\s+"([^"]+)"\s+\(([^)]+)\)\s+from\s+(.+?)\s*$'
)
MSVC_RAW_RE = re.compile(
    r'^\s*Discarded\s+(\S+)\s+from\s+(.+?)\s*$'
)
GNU_LD_RE = re.compile(
    r"^(?:[^:\s]*ld[^:]*:\s*)?removing unused section\s+'([^']+)'\s+in file\s+'([^']+)'\s*$"
)
LLD_RE = re.compile(
    r"^(?:[^:\s]*lld[^:]*:\s*)?removing unused section\s+(.+?):\(([^)]+)\)\s*$"
)

SECTION_PREFIXES = (
    ".text.hot.",
    ".text.unlikely.",
    ".text.startup.",
    ".text.exit.",
    ".text.",
    ".data.rel.ro.local.",
    ".data.rel.ro.",
    ".data.",
    ".rodata.",
    ".bss.",
)

FIRST_PARTY_LIBS = (
    "common",
    "core",
    "video_core",
    "audio_core",
    "hid_core",
    "input_common",
    "frontend_common",
    "shader_recompiler",
    "network",
    "mcp",
    "updater",
    "web_service",
    "dynarmic",
    "simd_kernels",
    "yuzu_cmd",
    "eden-cli",
    "eglantine",
)

THIRD_PARTY_PATH_MARKERS = (
    "/_deps/",
    "/externals/",
    "/.cache/",
)

THIRD_PARTY_LIB_MARKERS = (
    "boost",
    "fmt",
    "sdl3",
    "ffmpeg",
    "libav",
    "libsw",
    "openssl",
    "libssl",
    "libcrypto",
    "opus",
    "zstd",
    "lz4",
    "zlib",
    "catch2",
    "sirit",
    "xbyak",
    "enet",
    "cubeb",
    "libusb",
    "reshade",
    "vulkanheaders",
    "vulkanutilitylibraries",
    "vulkanmemoryallocator",
    "vulkan-1",
    "msvcp",
    "vcruntime",
    "ucrt",
    "msvcrt",
    "libcmt",
    "oldnames",
    "libstdc++",
    "libgcc",
)

NOISE_SUBSTRINGS = (
    "vftable",
    "vbtable",
    "vtable",
    "typeinfo",
    "scalar deleting destructor",
    "vector deleting destructor",
    "`string'",
    "??_C@",
    "??_7",
    "??_R",
    "??_G",
    "??_E",
    "_ZTV",
    "_ZTI",
    "_ZTS",
    "_ZThn",
    "_ZTv0",
    "__ehhandler",
    "__unwindfunclet",
    "$fin$",
    "$filt$",
    "$unwind$",
    "$pdata$",
    "$chain$",
    "$cppxdata$",
    "$ip2state$",
    "$stateUnwindMap$",
    "$tryMap$",
    "$handlerMap$",
    ".gsspr",
    ".gssep",
    "winrt::",
    "DirectX::",
    "thunk",
    ".eh_frame",
    ".debug_",
    ".pdata",
    ".xdata",
    "std::",
    "boost::",
    "fmt::",
    "nlohmann::",
    "Catch::",
    "spdlog::",
    "ImGui::",
    "vk::",
    "VK_",
    "_STL",
    "__gnu_cxx::",
    "__cxxabiv1::",
)

NOISE_PREFIXES = (
    "Java_",
    "JNI_",
    "SDL_App",
    "egl_",
)

ENTRY_EXACT = {"main", "wmain", "WinMain", "wWinMain", "_main", "_wmain"}


@dataclass(frozen=True)
class DeadSymbol:
    symbol: str
    mangled: str
    section: str
    source_file: str
    kind: str


def strip_section_prefix(section: str) -> str:
    for prefix in SECTION_PREFIXES:
        if section.startswith(prefix):
            return section[len(prefix):]
    return section


def classify_kind(symbol: str, section: str) -> str:
    if section.startswith(".text") or "(" in symbol:
        return "function"
    if section.startswith((".data", ".rodata", ".bss")):
        return "data"
    return "symbol"


def find_demangler() -> str | None:
    return shutil.which("llvm-cxxfilt") or shutil.which("c++filt")


def demangle_itanium(names: list[str]) -> dict[str, str]:
    if not names:
        return {}
    tool = find_demangler()
    if not tool:
        return {}
    unique = list(dict.fromkeys(names))
    try:
        proc = subprocess.run(
            [tool],
            input="\n".join(unique) + "\n",
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
            check=False,
        )
    except OSError:
        return {}
    if proc.returncode != 0:
        return {}
    out_lines = proc.stdout.splitlines()
    if len(out_lines) != len(unique):
        return {}
    return {raw: demangled.strip() or raw for raw, demangled in zip(unique, out_lines)}


def parse_linker_output(lines, *, demangle: bool = True) -> list[DeadSymbol]:
    raw_entries: list[DeadSymbol] = []
    for raw_line in lines:
        line = raw_line.rstrip("\r\n")
        if match := MSVC_QUOTED_RE.match(line):
            symbol, mangled, source = match.group(1), match.group(2), match.group(3)
            raw_entries.append(DeadSymbol(symbol, mangled, "", source, classify_kind(symbol, "")))
        elif match := MSVC_RAW_RE.match(line):
            mangled, source = match.group(1), match.group(2)
            raw_entries.append(DeadSymbol(mangled, mangled, "", source, classify_kind(mangled, "")))
        elif match := GNU_LD_RE.match(line):
            section, source = match.group(1), match.group(2)
            mangled = strip_section_prefix(section)
            raw_entries.append(
                DeadSymbol(mangled, mangled, section, source, classify_kind(mangled, section))
            )
        elif match := LLD_RE.match(line):
            source, section = match.group(1), match.group(2)
            mangled = strip_section_prefix(section)
            raw_entries.append(
                DeadSymbol(mangled, mangled, section, source, classify_kind(mangled, section))
            )

    if not demangle or not raw_entries:
        return raw_entries

    to_demangle = [
        e.mangled for e in raw_entries if e.symbol == e.mangled and e.mangled.startswith("_Z")
    ]
    demangled_map = demangle_itanium(to_demangle)
    if not demangled_map:
        return raw_entries

    resolved: list[DeadSymbol] = []
    for entry in raw_entries:
        if entry.symbol == entry.mangled and entry.mangled in demangled_map:
            sym = demangled_map[entry.mangled]
            resolved.append(
                DeadSymbol(
                    symbol=sym,
                    mangled=entry.mangled,
                    section=entry.section,
                    source_file=entry.source_file,
                    kind=classify_kind(sym, entry.section),
                )
            )
        else:
            resolved.append(entry)
    return resolved


def is_first_party_source(source_file: str) -> bool:
    norm = source_file.replace("\\", "/").lower()
    padded = f"/{norm}"
    if any(marker in padded for marker in THIRD_PARTY_PATH_MARKERS):
        return False

    # Strip archive member e.g. path/to/core.lib(foo.cpp.obj) -> path/to/core.lib
    archive_part = norm.split("(", 1)[0]
    archive_base = os.path.basename(archive_part)
    archive_stem = archive_base
    for suffix in (".lib", ".a", ".so", ".dylib", ".dll", ".obj", ".o"):
        if archive_stem.endswith(suffix):
            archive_stem = archive_stem[: -len(suffix)]
            break

    if archive_stem.startswith("lib") and len(archive_stem) > 3:
        bare_stem = archive_stem[3:]
    else:
        bare_stem = archive_stem

    if any(marker in archive_base for marker in THIRD_PARTY_LIB_MARKERS):
        return False
    if archive_base == "libc.a" or archive_base.startswith("libc."):
        return False

    if "/src/" in padded or norm.startswith("src/"):
        return True
    if archive_stem in FIRST_PARTY_LIBS or bare_stem in FIRST_PARTY_LIBS:
        return True
    for part in archive_part.split("/"):
        if part.endswith(".dir") and part[:-4] in FIRST_PARTY_LIBS:
            return True
    return False


def is_noise_symbol(entry: DeadSymbol) -> bool:
    for text in (entry.symbol, entry.mangled, entry.section):
        if not text:
            continue
        if any(sub in text for sub in NOISE_SUBSTRINGS):
            return True
        if any(text.startswith(prefix) for prefix in NOISE_PREFIXES):
            return True

    base_name = entry.symbol.split("(", 1)[0].strip()
    ident = base_name.split()[-1] if base_name else ""
    if any(ident.startswith(prefix) for prefix in NOISE_PREFIXES):
        return True
    if base_name in ENTRY_EXACT or ident in ENTRY_EXACT or entry.mangled in ENTRY_EXACT:
        return True
    return False


def filter_entries(
    entries: list[DeadSymbol],
    *,
    first_party_only: bool = True,
    exclude_noise: bool = True,
    substring_filter: str = "",
) -> list[DeadSymbol]:
    needle = substring_filter.lower()
    seen: set[tuple[str, str]] = set()
    filtered: list[DeadSymbol] = []
    for entry in entries:
        if first_party_only and not is_first_party_source(entry.source_file):
            continue
        if exclude_noise and is_noise_symbol(entry):
            continue
        if needle and needle not in entry.symbol.lower() and needle not in entry.source_file.lower():
            continue
        key = (entry.source_file, entry.symbol)
        if key in seen:
            continue
        seen.add(key)
        filtered.append(entry)
    return filtered


def format_report(entries: list[DeadSymbol], *, max_per_object: int = 20) -> str:
    if not entries:
        return "No dead first-party symbols found.\n"
    grouped: dict[str, list[DeadSymbol]] = defaultdict(list)
    for entry in entries:
        grouped[entry.source_file].append(entry)

    ordered = sorted(grouped.items(), key=lambda item: (-len(item[1]), item[0]))
    lines = [f"Found {len(entries)} dead symbol(s) across {len(ordered)} object/library unit(s):"]
    for source_file, items in ordered:
        lines.append(f"\n[{len(items)}] {source_file}")
        limit = len(items) if max_per_object <= 0 else max_per_object
        for item in items[:limit]:
            lines.append(f"  - ({item.kind}) {item.symbol}")
        if len(items) > limit:
            lines.append(f"  ... and {len(items) - limit} more")
    return "\n".join(lines) + "\n"


def add_filter_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--include-noise", action="store_true", help="keep ABI/template/FFI symbols")
    parser.add_argument("--all-sources", action="store_true", help="include third-party/system objects")
    parser.add_argument("--filter", default="", help="case-insensitive substring filter")
    parser.add_argument("--max-per-object", type=int, default=20, help="cap symbols printed per unit (0=all)")
    parser.add_argument("--json", action="store_true", help="emit JSON array")
    parser.add_argument("--no-demangle", action="store_true", help="skip c++filt/llvm-cxxfilt demangling")
    parser.add_argument(
        "--fail-on-findings",
        action="store_true",
        help="exit 1 if any dead symbols remain after filtering",
    )


def emit_results(entries: list[DeadSymbol], args: argparse.Namespace) -> int:
    filtered = filter_entries(
        entries,
        first_party_only=not args.all_sources,
        exclude_noise=not args.include_noise,
        substring_filter=args.filter,
    )
    if args.json:
        print(json.dumps([asdict(e) for e in filtered], indent=2))
    else:
        sys.stdout.write(format_report(filtered, max_per_object=args.max_per_object))
    if args.fail_on_findings and filtered:
        return 1
    return 0


def cmd_parse(args: argparse.Namespace) -> int:
    if not args.logfile or args.logfile == "-":
        lines = sys.stdin.read().splitlines()
    else:
        with open(args.logfile, encoding="utf-8", errors="replace") as handle:
            lines = handle.read().splitlines()
    entries = parse_linker_output(lines, demangle=not args.no_demangle)
    return emit_results(entries, args)


def cmd_run(args: argparse.Namespace) -> int:
    command = list(args.cmd)
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        sys.exit("Usage: dead_code.py run [options] -- <build/link command...>")
    proc = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        return proc.returncode
    entries = parse_linker_output(proc.stdout.splitlines(), demangle=not args.no_demangle)
    return emit_results(entries, args)


def cmd_static(args: argparse.Namespace) -> int:
    clang_tidy = shutil.which("clang-tidy")
    cppcheck = shutil.which("cppcheck")
    if not clang_tidy and not cppcheck:
        print(
            "Neither clang-tidy nor cppcheck was found on PATH. "
            "Install one of them or use linker GC reporting via -DENABLE_DEAD_CODE_REPORT=ON.",
            file=sys.stderr,
        )
        return 1

    build_dir = Path(args.build_dir)
    compile_db = build_dir / "compile_commands.json"
    exit_code = 0

    if cppcheck:
        cmd = [cppcheck, "--enable=unusedFunction", "--quiet", *args.paths]
        print(f"Running: {' '.join(cmd)}")
        rc = subprocess.run(cmd, check=False).returncode
        if rc != 0:
            exit_code = rc

    if clang_tidy:
        if not compile_db.is_file():
            print(
                f"Skipping clang-tidy: {compile_db} not found (configure CMake first).",
                file=sys.stderr,
            )
        else:
            checks = "-*,misc-unused-using-decls,misc-unused-parameters,clang-analyzer-deadcode.*"
            sources = []
            for path_str in args.paths:
                p = Path(path_str)
                if p.is_file():
                    sources.append(str(p))
                elif p.is_dir():
                    sources.extend(str(f) for f in sorted(p.rglob("*.cpp")))
            if sources:
                cmd = [clang_tidy, "-p", str(build_dir), f"-checks={checks}", *sources[:100]]
                print(f"Running clang-tidy on {min(len(sources), 100)} translation unit(s)...")
                rc = subprocess.run(cmd, check=False).returncode
                if rc != 0:
                    exit_code = rc

    return exit_code


def main() -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    sub = parser.add_subparsers(dest="subcommand", required=True)

    parse_p = sub.add_parser("parse", help="parse MSVC /VERBOSE:REF or ld/lld --print-gc-sections output")
    parse_p.add_argument("logfile", nargs="?", default="-", help="linker log file (default: stdin)")
    add_filter_arguments(parse_p)
    parse_p.set_defaults(func=cmd_parse)

    run_p = sub.add_parser("run", help="run a build/link command and report dead symbols")
    add_filter_arguments(run_p)
    run_p.add_argument("cmd", nargs=argparse.REMAINDER, help="command after --")
    run_p.set_defaults(func=cmd_run)

    static_p = sub.add_parser("static", help="run cppcheck / clang-tidy unused-code checks")
    static_p.add_argument("--build-dir", default="build", help="CMake build dir containing compile_commands.json")
    static_p.add_argument("--paths", nargs="+", default=["src"], help="source paths to analyze")
    static_p.set_defaults(func=cmd_static)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
