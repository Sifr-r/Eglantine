# Tools

Run commands from the repository root unless the tool's guide gives a different working directory.

| Task | Entry point | Prerequisites and effects |
| --- | --- | --- |
| Diagnose a running emulator | [Built-in MCP server](./mcp/README.md) | Enable the loopback server in the frontend. Start with status and read-only tools. |
| Diagnose Switch 2 rendering | [NX2 lab automation](../.claude/skills/nx2-lab-automation/SKILL.md) and [shader triage](../.claude/skills/nx2-shader-triage/SKILL.md) | Use an existing live debug session or a bounded `eden-cli` launch. |
| Find an identified native shader | [Shader catalogue](./nx2/catalog/README.md) | Python 3. Lookups read the repo and local copies. |
| Check release inputs | [CI and release checks](#ci-and-release-checks) | Python 3. Scans paths and archive member names. |
| Inspect or update dependency pins | [CPMUtil](./cpm/README.md) | POSIX shell and CMake 3.31 or newer. Inspect `cpmfile.json` before changing pins. |
| Refresh translations or icons | [Translation scripts](./translations/README.md) and [icon inputs](../dist/icon_variations/README.md) | Asset scripts rewrite source or generated files. Review the resulting diff. |

Keep shell scripts POSIX compliant and use `#!/bin/sh -e`. Use
`#!/usr/bin/env <interpreter>` for Python, Ruby, or Perl scripts.

## Binaries

Enable these CMake targets with `-DENABLE_DEBUG_TOOLS=ON`.

- `maxwell-spirv` converts Maxwell shader dumps to raw SPIR-V on standard output.
- `maxwell-disas` disassembles Maxwell shader dumps.
- `maxwell-ir` prints the generated intermediate representation for a Maxwell shader.

## Switch 2 and emulator debugging

- [`nx2/lab_ctl.py`](./nx2/lab_ctl.py) manages scripted `eden-cli` diagnostics runs. Set
  `--max-seconds` on `launch` and call `stop` on your instance when finished. Keep the
  same `-d` diagnostics directory for both commands. See the
  [lab automation guide](../.claude/skills/nx2-lab-automation/SKILL.md).
- [`nx2/shader_lab.py`](./nx2/shader_lab.py) opens a browser interface for shader diagnostics. See the [shader triage guide](../.claude/skills/nx2-shader-triage/SKILL.md) and [catalogue format](./nx2/catalog/README.md).
- [`nx2/debug_mcp.py`](./nx2/debug_mcp.py) connects to the [NX2 live debug server](../docs/content/nx2/debug-mcp.md).
- [Built-in MCP server](./mcp/README.md) documents the emulator's separate MCP endpoint and its command-line client.

## CI and release checks

- [`ci/check_release_paths.py`](./ci/check_release_paths.py) scans NUL-delimited Git paths (`--stdin0`) or the members of ZIP and TAR.GZ archives (`--archives`) for private artifacts.
  A clean scan exits 0. Rejected paths, malformed path input, and unreadable archives exit 1.

```sh
python3 -I tools/ci/check_release_paths.py --archives path/to/release.zip
python3 -m unittest discover -s tools/ci -p test_check_release_paths.py
```

## Scripts

- `generate_converters.py`: Generates converters for given formats of textures (C++ helper).
- `svc_generator.py`: Generates the files `src/core/hle/kernel/svc.cpp` and `src/core/hle/kernel/svc.h` based off prototypes.
- `shellcheck.sh` runs ShellCheck in POSIX `sh` mode on shell scripts under `tools/`.
- `llvmpipe-run.sh` sets Mesa environment variables and runs a command with software rendering.
- `optimize-assets.sh`: Optimizes PNG assets with OptiPng.
- [`update-icons.sh`](./update-icons.sh) regenerates icons and the built-in avatar. The
  [icon guide](../dist/icon_variations/README.md) lists inputs, tools, and output paths.
- `dtrace-tool.pl` profiles a running process with DTrace and generates a FlameGraph.
- `lanczos-gen.pl`: Generates constants for the Lanczos filter.
- `clang-format.sh`: Runs `clang-format` on the entire codebase.
    * Requires: clang
- `cpp-lint.sh`: Homemade dumb C++ linter.
- `dead_code.py`: Parses MSVC (`/VERBOSE:REF` via `-DENABLE_DEAD_CODE_REPORT=ON`) or GNU `ld`/`lld` (`--print-gc-sections`) linker GC output, filters third-party/ABI noise, and optionally runs `cppcheck`/`clang-tidy` unused checks.
- `fuzzsettings.cpp`: Fuzz settings files.

## Android

Use these scripts when cleaning up Android string resources. Both edit resource files in
place, so review their changes before including the output.

- [`unused-strings.sh`](./unused-strings.sh) removes unused definitions from Android
  `strings.xml` files. It uses Python 3 as well as shell utilities.
- [`stale-translations.sh`](./stale-translations.sh) removes localized strings that lack
  a definition in the source `values/strings.xml`.
