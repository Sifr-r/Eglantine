# Tools

Tools for Eglantine development and maintenance. Keep shell scripts POSIX compliant and use `#!/bin/sh -e`. Use `#!/usr/bin/env <interpreter>` for Python, Ruby, or Perl scripts.

## Third-Party

- [CPMUtil scripts](./cpm/README.md) for dependency maintenance. Run `tools/cpmutil.sh update` to update CPMUtil and its tooling.

## Binaries

Enable these CMake targets with `-DENABLE_DEBUG_TOOLS=ON`.

- `maxwell-spirv` converts Maxwell shader dumps to raw SPIR-V on standard output.
- `maxwell-disas` disassembles Maxwell shader dumps.
- `maxwell-ir` prints the generated intermediate representation for a Maxwell shader.

## Switch 2 and emulator debugging

- [`nx2/lab_ctl.py`](./nx2/lab_ctl.py) manages scripted `eden-cli` diagnostics runs. Set `--max-seconds` when launching and use the `stop` subcommand when finished. See the [lab automation guide](../.claude/skills/nx2-lab-automation/SKILL.md).
- [`nx2/shader_lab.py`](./nx2/shader_lab.py) opens a browser interface for shader diagnostics. See the [shader triage guide](../.claude/skills/nx2-shader-triage/SKILL.md) and [catalogue format](./nx2/catalog/README.md).
- [`nx2/debug_mcp.py`](./nx2/debug_mcp.py) connects to the [NX2 live debug server](https://github.com/Eglantine-Community/docs/blob/master/content/nx2/debug-mcp.mdx).
- [Built-in MCP server](./mcp/README.md) documents the emulator's separate MCP endpoint and its command-line client.

## CI and release checks

- [`ci/check_release_paths.py`](./ci/check_release_paths.py) scans NUL-delimited Git paths (`--stdin0`) or the members of ZIP and TAR.GZ archives (`--archives`) for private artifacts.

## Scripts

- `generate_converters.py`: Generates converters for given formats of textures (C++ helper).
- `svc_generator.py`: Generates the files `src/core/hle/kernel/svc.cpp` and `src/core/hle/kernel/svc.h` based off prototypes.
- `shellcheck.sh` runs ShellCheck in POSIX `sh` mode on shell scripts under `tools/`.
- `llvmpipe-run.sh` sets Mesa environment variables and runs a command with software rendering.
- `optimize-assets.sh`: Optimizes PNG assets with OptiPng.
- `update-icons.sh` regenerates desktop and Android icons from `dist/icon_variations/${VARIATION}.svg` (`base` by default) and updates the built-in avatar. It uses ImageMagick, OptiPNG, and Python 3, with `png2icns` for the macOS icon.
- `dtrace-tool.pl` profiles a running process with DTrace and generates a FlameGraph.
- `lanczos-gen.pl`: Generates constants for the Lanczos filter.
- `clang-format.sh`: Runs `clang-format` on the entire codebase.
    * Requires: clang
- `cpp-lint.sh`: Homemade dumb C++ linter.
- `fuzzsettings.cpp`: Fuzz settings files.

## Android

It's recommended to run these scripts after almost any Android change, as they are relatively fast and important both for APK bloat and CI.

- `unused-strings.sh`: Finds unused strings in `strings.xml` files.
- `stale-translations.sh`: Finds translated strings that aren't present in the source `strings.xml` file.

## Translations

- [Translation scripts](./translations/README.md)
