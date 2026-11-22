# Polyglot migration guide

Eglantine currently lowers Switch 2 Ampere shaders in C++. The first migration candidate is a standalone Rust implementation of that lowering step. The Cargo workspace, C ABI header, CMake helper and differential harness exist. The emulator build does not invoke the helper or select the Rust implementation. Start with the [baseline procedure](migration_docs/00_PREREQUISITES_AND_BASELINE.md), then follow the [Phase 1A integration guide](migration_docs/01_PHASE1A_RUST_AMPERE_PARITY.md).

## What works today

From the repository root, run the standalone Rust suite and build its static library:

```sh
cargo test --manifest-path rust/Cargo.toml --locked --offline
cargo build --manifest-path rust/Cargo.toml --release --locked --offline
```

The workspace currently has a single std-only crate. On Windows with the native MSVC target and Cargo’s default output directory, the release archive is `rust/target/release/eglantine_ampere_rs.lib`. [Workspace manifest](rust/Cargo.toml) · [Public C ABI](rust/include/eglantine_ampere.h)

| Check | Windows result on 2026-11-22 | What it covers |
|---|---|---|
| Cargo integration tests | 7 passed | Capture, lowering, address maps, overrides and FFI behavior. |
| Standalone differential harness (rebuilt with MSVC) | 6,200 cases, 0 mismatches | Compares Rust and C++ for the harness inputs, including rejected programs. Rebuild the harness against both implementations after changing either one. |
| C++ Catch2 `[ampere]` (existing local build) | 70 cases, 60,262 assertions passed | Exercises the C++ reference path. Integrated backend selection needs a separate test once it exists. |

## Phase guides

The status column identifies the next work. Paths, switches and tests described as proposals become usable when their phase lands. The packaging guide applies to each phase that introduces a build dependency.

| Guide | Status and scope | Evidence needed before rollout |
|---|---|---|
| [0 · Baseline](migration_docs/00_PREREQUISITES_AND_BASELINE.md) | Available today. Record the toolchain and reference results. | Repeatable inputs and a passing reference suite. |
| [1A · Rust Ampere parity](migration_docs/01_PHASE1A_RUST_AMPERE_PARITY.md) | Prototype exists. Wire it into the build and the C++ lowering entry point. | Matching outputs and errors, plus a working C++-only build. |
| [1B · Direct SM86 to SPIR-V](migration_docs/02_PHASE1B_DIRECT_SM86_SPIRV.md) | Proposed compiler and immutable shader inputs. Depends on the Rust build integration. | Validate shader modules, compare rendering and exercise fallback and cache compatibility. |
| [2 · Shaders, upscaling and video](migration_docs/03_PHASE2_SLANG_TENSOR_UPSCALERS_VIDEO.md) | Proposed Slang shader build, matrix calibration, temporal upscaler interface and Vulkan Video path. | Compare outputs on supported hardware and exercise existing paths on unsupported devices. |
| [3 · CPU and Vulkan hot paths](migration_docs/04_PHASE3_ISPC_CPP_HOTPATH_ARM64.md) | Proposed SIMD kernels, renderer and scheduler changes, and ARM64 backends. Profile each independently. | Reference-output tests and repeatable performance measurements on the affected architecture. |
| [4 · Console experience](migration_docs/05_PHASE4_CONSOLE_EXPERIENCE.md) | Proposed controller transport, NUP archive implementation and Slint frontend. | Controller reconnect, archive interoperability and frontend lifecycle checks. |
| [Build, CI and packaging](migration_docs/06_BUILD_CI_PACKAGING_AND_CHECKLIST.md) | Current build responsibilities and proposed changes by platform. | Build with each new dependency enabled and disabled, and check the resulting packages. |

## Integration boundaries

- Keep emulator state, guest memory access and locking in C++. Pass owned snapshots or explicitly scoped buffers to workers. Document pointer lengths, lifetimes, allocator ownership and error returns in the C ABI.
- Put boundaries around a shader program, decode buffer or archive batch. Keep per-draw and per-instruction hot loops within one implementation.
- Keep the existing path available during rollout. Compare both implementations on the same inputs before enabling a replacement by default. For side effects such as controller writes, compare recorded inputs or outputs offline.
- Add a toolchain dependency only to builds that enable its feature. Pin and vendor new dependencies where the package build must run offline.
