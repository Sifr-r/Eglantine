# Phase 0 — Establish a reproducible baseline

Run these steps from the repository root before integrating a migration phase. Record the revision (`git log -1 --oneline`), relevant local changes (`git diff` and `git status --short`), OS, architecture, compiler, build type and feature settings with the results. Keep unrelated work in place. A baseline applies to the exact working tree and inputs you measured.

## Build the reference and the prototype

Use the repository’s configured CMake build directory. The target names are the same on Windows and Unix. [Project instructions](../AGENTS.md)

```sh
cmake --build build --target yuzu yuzu-cmd tests --parallel
```

For Phase 1A, also record `rustc -vV` and `cargo --version`. The standalone workspace uses the standard library and its checked-in lockfile. Run both commands from the repository root:

```sh
cargo test --manifest-path rust/Cargo.toml --locked --offline
cargo build --manifest-path rust/Cargo.toml --release --locked --offline
```

Use a Rust target compatible with the C++ compiler, architecture and runtime when linking the harness. On Windows, run MSVC commands from a matching Visual Studio developer shell. The CMake helper’s target-selection rules and the outstanding MSYS2 and cross-build checks are described in [Phase 1A](01_PHASE1A_RUST_AMPERE_PARITY.md). Later phases introduce their own prerequisites. See the [platform build checklist](06_BUILD_CI_PACKAGING_AND_CHECKLIST.md) before enabling a new dependency in packaging.

## Run the checks for the affected subsystem

Use `./build/bin/tests` on Unix or `.\build\bin\tests.exe` on Windows, followed by the quoted filter. Pass each filter separately and record the test count and exit status.

| Area | Catch2 filter | What to record |
|---|---|---|
| Ampere lowering | `"[ampere]"` | Reference pass/fail and assertion count. Compare Rust and C++ outputs separately with the [differential harness](../rust/tools/diff_check/diff_check.cpp). |
| Shader compilation latency | `"[.shader_bench]"` | Program count, failures, stage timings and p50/p90/p99 per-program time for the same dump set. |
| Switch 2 controller protocol and pairing | `"[switch2]"` | Protocol and pairing results before changing transport. |
| NUP archives | `"[nup]"` | Reader and writer results before changing the container implementation. |
| Native macro engine and GPU performance | `"[native_macro]"`, `"[nx2_perf]"` | Results for the affected macro and renderer paths. |

The shader benchmark requires a directory of local native shader dumps. Set `EGLANTINE_SHADER_BENCH` to that directory in the test process environment. Without it Catch2 skips the case. The benchmark prints the number of failed programs separately from its process exit status. Set `EGLANTINE_SHADER_BENCH_OUT` to a local file to compare per-program outcomes across runs. [Benchmark implementation](../src/tests/video_core/shader_bench.cpp)

## Compare a gameplay profile

Use the same game version, scene, renderer settings, driver, cache state and input sequence before and after the change. Record the profiling interval and compare frame-time percentiles, pipeline stalls or GPU thread time according to the feature being changed. Keep timing data and dumps local.

For an unattended run, use a dedicated diagnostics directory and a positive timeout. The example uses PowerShell variables for the local game path and a fresh diagnostics directory. Run the stop command for the instance you launched, including after an interrupted measurement. [Lab controller](../tools/nx2/lab_ctl.py)

```powershell
python -I tools/nx2/lab_ctl.py -d "$diagDir" launch --binary build/bin/eden-cli.exe --game "$gamePath" --hidden --env EDEN_NX2_GPU_PROFILE=1 --max-seconds 180
python -I tools/nx2/lab_ctl.py -d "$diagDir" state
# Collect the profile for the selected interval before stopping.
python -I tools/nx2/lab_ctl.py -d "$diagDir" stop
```

## Before submitting changes

- Keep game files, credentials, shader binaries, frame dumps and local profiling artifacts out of release inputs. [Release path checker](../tools/ci/check_release_paths.py)
- Before each commit, run `python -I tools/nx2/catalog.py status`. Exit 1 means local entries need merging. Follow the repository’s separate catalogue commit procedure and include only program metadata. [Catalogue instructions](../AGENTS.md)
- Use the project SPDX headers for new source files. The packaging checklist identifies the extensions that need checker support as new languages are introduced. [Header checker](../tools/ci/license-header.sh)
- Review the coding and AI contribution policies before submission. Human authors review their changes, write their commit and PR text, and disclose AI assistance in both. [Coding guidelines](../docs/content/policies/Coding.mdx) · [AI policy](../docs/content/policies/AI.mdx)
