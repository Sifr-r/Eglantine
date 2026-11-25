# `build_ci` — Debugging Toolchain, MCP, Local Storage, and CI/Release

Entry points: `CMakeLists.txt`, `tools/README.md`, `.github/workflows/source-checks.yml`, `.github/workflows/release.yml`.

## Switch 2 Debugging Toolchain (`tools/nx2/` & `mcp`)

| Tool / Interface | Purpose | Guide |
|---|---|---|
| `tools/nx2/lab_ctl.py` | Unattended `eden-cli` runs (`--max-seconds` on `launch`, `stop` when done) | [nx2-lab-automation](../../.claude/skills/nx2-lab-automation/SKILL.md) |
| `tools/nx2/shader_lab.py` & `catalog.py` | Shader diagnostics UI + per-game shader catalogue (`status` / `merge` pre-commit) | [nx2-shader-triage](../../.claude/skills/nx2-shader-triage/SKILL.md), [catalog README](../../tools/nx2/catalog/README.md) |
| `tools/nx2/debug_mcp.py` | Live debug server adapter (`EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545`) | [nx2-debug-server](../../.claude/skills/nx2-debug-server/SKILL.md), [debug-mcp doc](../content/nx2/debug-mcp.md) |
| `tools/nx2/nvd_cubin.py` | Disassembles dumped Ampere shader programs | — |
| `src/mcp/tools/` & `mcp_host_cli.cpp` | Built-in emulator MCP server (`ENABLE_MCP_SERVER`) | [MCP README](../../tools/mcp/README.md) |

## Local-Only Storage (`ignore/` & `.lab/`)

- `ignore/` holds local-only bisect harnesses (`ignore/.lab/build.cmd`, `nx2_gameplay_baseline.py`), quarantined fixtures (`ignore/fixtures/`, excluded from normal `src/tests/`), quarantined legacy assets (`ignore/provenance/`), and point-in-time review snapshots.
- Git, Nix source filters, and `CPackSourceConfig.cmake.in` exclude `ignore/` and `.lab/` from tracked sources and source packages.

## Source, License, and Release Checks

| Path | Responsibility |
|---|---|
| `ASSET_PROVENANCE.md` | Source evidence, attribution, and open redistribution questions for audited assets. |
| `CPackSourceConfig.cmake.in` | Excludes root `ignore/` and `.lab/` from CPack source archives. |
| `tools/ci/check_release_paths.py` | Rejects private artifact paths in `--stdin0` Git lists or `--archives` ZIP/TAR.GZ members (`test_check_release_paths.py`). |
| `tools/ci/check_docs_layout.py` | Rejects a documentation tree that is a submodule pointer, a nested submodule, or missing its entry point and knowledge cards, so the docs stay in this repository (`test_check_docs_layout.py`). |
| `tools/ci/license-header.sh` | Checks/repairs SPDX headers on diffs against `nx2` (excludes third-party trees like `externals/FidelityFX-SDK`; `test_license_header.py`). |
| `tools/ci/test_workflows.py` | Validates `.github/workflows` & `.github/actions` triggers, timeouts, cache wiring, and hygiene. |
| `.github/workflows/test-linux.yml` | Reusable headless Linux Catch2 + Python/script check workflow (`ccache` + `.cache/cpm`). |
| `.github/workflows/source-checks.yml` | Runs tree scan, documentation-layout check, `license-header.sh` on PRs, and `test-linux.yml` on `push` (`nx2`, `nx2-experimental`) / `pull_request`. |
| `.github/workflows/build-android.yml` | Android Gradle build (`arm64-v8a` with `adrenotools` + `eglantine_rs` via NDK Clang). |
| `.github/workflows/release.yml` | Trusted-checkout scan of build commit + ZIP/TAR.GZ/AppImage/macOS bundle (`dist/eglantine.icns` + licenses/asset record). |
| `.github/workflows/obsolete-on-release.yml` | Marks unpinned/unassigned/unmilestoned open issues obsolete post-release. |

## Verification

- `python -I tools/ci/test_check_release_paths.py` and `python -I tools/ci/test_workflows.py`
- `python -I tools/ci/test_check_docs_layout.py` and `python3 -I tools/ci/check_docs_layout.py --ref HEAD`
- `git diff --cached --name-only --diff-filter=ACMR -z | python3 -I tools/ci/check_release_paths.py --stdin0`
