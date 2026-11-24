# Historical polyglot migration guide

This document records the original migration proposal and its November 2026 baseline.
See [first-party language and domain ownership](ARCHITECTURE.md#first-party-language-and-domain-ownership)
for the current build and implementation decisions.

At the original baseline, Eglantine lowered Switch 2 Ampere shaders in C++. The standalone Rust prototype, C ABI header, CMake helper and differential harness existed before the emulator linked them. The [current CMake build](CMakeLists.txt) can link the Rust FFI bundle, and C++ remains the default lowering path. Use the [baseline procedure](migration_docs/00_PREREQUISITES_AND_BASELINE.md) to compare revisions.

## Recorded baseline

The original check ran the standalone suite and built its static library from the repository root:

```sh
cargo test --manifest-path rust/Cargo.toml --locked --offline
cargo build --manifest-path rust/Cargo.toml --release --locked --offline
```

At that revision the workspace had one std-only crate. On Windows with the native MSVC target and Cargo’s default output directory, its release archive was `rust/target/release/eglantine_ampere_rs.lib`. The current [workspace manifest](rust/Cargo.toml) and [architecture ledger](ARCHITECTURE.md#first-party-language-and-domain-ownership) describe the integrated crates and archive.

| Check | Windows result on 2026-11-22 | What it covers |
|---|---|---|
| Cargo integration tests | 7 passed | Capture, lowering, address maps, overrides and FFI behavior. |
| Standalone differential harness (rebuilt with MSVC) | 6,200 cases, 0 mismatches | Compares Rust and C++ for the harness inputs, including rejected programs. Rebuild the harness against both implementations after changing either one. |
| C++ Catch2 `[ampere]` (existing local build) | 70 cases, 60,262 assertions passed | C++ reference path before integration. See the [current parity tests](src/tests/video_core/ampere_rust_parity.cpp) for integrated backend selection. |

## Phase guides

The phase guides retain the original proposals and point to their current implementation where available. Use the architecture ledger to determine ownership and which paths are enabled.

| Guide | Status and scope | Evidence needed before rollout |
|---|---|---|
| [0 · Baseline](migration_docs/00_PREREQUISITES_AND_BASELINE.md) | Verification procedure for the selected revision. | Repeatable inputs and a passing reference suite. |
| [1A · Rust Ampere parity](migration_docs/01_PHASE1A_RUST_AMPERE_PARITY.md) | Integrated and opt-in at runtime. | Matching outputs and errors, plus a working C++-only build. |
| [1B · Direct SM86 to SPIR-V](migration_docs/02_PHASE1B_DIRECT_SM86_SPIRV.md) | Integrated opt-in path for eligible compute shaders. | Validate shader modules, compare rendering and exercise fallback and cache compatibility. |
| [2 · Shaders, upscaling and video](migration_docs/03_PHASE2_SLANG_TENSOR_UPSCALERS_VIDEO.md) | Matrix calibration and shared temporal interface exist. Slang scaffolding was removed. Vulkan Video is staged. | Compare outputs on supported hardware and exercise existing paths on unsupported devices. |
| [3 · CPU and Vulkan hot paths](migration_docs/04_PHASE3_ISPC_CPP_HOTPATH_ARM64.md) | ISPC texture kernels and ARM64 macro JIT exist. Other experiments need independent evidence. | Reference-output tests and repeatable performance measurements on the affected architecture. |
| [4 · Console experience](migration_docs/05_PHASE4_CONSOLE_EXPERIENCE.md) | Optional Rust NUP and controller codec implementations exist. Portable transport and the Slint frontend remain proposals. | Controller reconnect, archive interoperability and frontend lifecycle checks. |
| [Build, CI and packaging](migration_docs/06_BUILD_CI_PACKAGING_AND_CHECKLIST.md) | Historical file matrix and links to the current build and release checks. | Build with each new dependency enabled and disabled, and check the resulting packages. |

## Integration boundaries

- Keep emulator state, guest memory access and locking in C++. Pass owned snapshots or explicitly scoped buffers to workers. Document pointer lengths, lifetimes, allocator ownership and error returns in the C ABI.
- Put boundaries around a shader program, decode buffer or archive batch. Keep per-draw and per-instruction hot loops within one implementation.
- Keep the existing path available during rollout. Compare both implementations on the same inputs before enabling a replacement by default. For side effects such as controller writes, compare recorded inputs or outputs offline.
- Add a toolchain dependency only to builds that enable its feature. Pin and vendor new dependencies where the package build must run offline.
