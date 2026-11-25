# `rust_ffi` & `simd_kernels` — Ownership, Cargo Workspace, and ISPC

## First-party language and domain ownership

| Area | Entry points | Decision |
|---|---|---|
| Guest runtime & desktop host | `src/core`, `src/video_core`, `src/input_common`, `src/yuzu_cmd`, `rust/apps/eglantine_gpui` | C++ owns emulator state and SDL host; GPUI (`eglantine`) owns desktop shell; keep ARM asm for NCE/Dynarmic and macOS Dynarmic C exception bridge. |
| Guest shaders | `src/shader_recompiler`, `rust/crates/ampere_{core,spirv}` | C++ default for Ampere->Maxwell lowering; Rust `ampere_core` opt-in (`EDEN_NX2_RUST_AMPERE`); Rust `ampere_spirv` opt-in for direct SM86 compute (`EDEN_NX2_DIRECT_SM86`). |
| NUP archives | `src/core/file_sys`, `rust/crates/nup_fs` | C++ default reader/writer pending Rust parity on malformed journal recovery, destructive pack, and legacy archives. |
| Switch 2 input | `src/input_common`, `rust/crates/switch2_io` | C++ default codec/transport pending Rust USB/BLE error, reconnect, and rumble parity. |
| Host shaders & SIMD | `src/video_core/host_shaders`, `dist/post_shaders`, `src/simd_kernels` | Keep GLSL build + post shaders (Slang & ISPC audio retired). Keep 3 ISPC texture kernels until x86-64 + ARM64 throughput/parity gates pass. |
| Android frontend | `src/android`, `src/common/android`, `src/common/fs/fs_android.*` | Kotlin/Android APIs + JNI at C++ boundary; Gradle Kotlin DSL. |
| Build, CI, tools | CMake, Gradle, Cargo, `.github/`, `tools/` | Keep declarative build configs, Python release/CI checks, Perl DTrace/filters, and C crash helper. |

Retire a duplicate only after identical outputs, failure behavior, and acceptable performance (prefer C++ on a tie; verify desktop + ARM64 Android link before removing C++).

## Cargo Workspace (`rust/`, `std`-only, zero external `crates.io` deps in `rust/Cargo.lock`)

| Crate | Path | C ABI Header & Responsibility |
|---|---|---|
| `eglantine_ampere_core` | `rust/crates/ampere_core` | `rust/include/eglantine_ampere.h` — SM86->Maxwell lowering, capture state machine, native address map, override codec |
| `eglantine_ampere_spirv` | `rust/crates/ampere_spirv` | `rust/include/eglantine_ampere_spirv.h` — SM86->SSA IR->SPIR-V 1.6 emitter (`OpTypeCooperativeMatrixKHR`, `std::thread::scope`) |
| `eglantine_nup_fs` | `rust/crates/nup_fs` | `rust/include/eglantine_nup_fs.h` — `NUP\x01` parser/writer/editor, `.part`/`.journal` resume via `EglNupZstdOps`/`EglNupVfsOps` |
| `eglantine_switch2_io` | `rust/crates/switch2_io` | `rust/include/eglantine_switch2_io.h` — Switch 2 BLE/USB framing, sticks, gyro/mouse, HD Rumble 2 |
| `eglantine_ffi` | `rust/crates/eglantine_ffi` | Unified `staticlib` (`eglantine_rs`) exporting 57 `extern "C"` symbols via feature-gated `#[used]` anchors |

- **Crate & CMake DAG:** `eglantine_ffi` -> `{ampere_core, ampere_spirv, nup_fs, switch2_io}` and `ampere_spirv` -> `ampere_core` (acyclic). Root `CMakeLists.txt` enables `ENABLE_RUST_{AMPERE,AMP_SPIRV,NUP,SWITCH2_IO}` (`ON` by default). `CMakeModules/AddRustCrate.cmake` wires `eglantine_rs` (`--no-default-features` + enabled `--features`) before `add_subdirectory(src)`: `shader_recompiler` (`AMPERE`, `AMP_SPIRV`), `core` (`NUP` + `zstd::zstd`), `input_common` (`SWITCH2_IO`), and `tests`.
- **C ABI & Android NDK:** Crossings use `#[repr(C)]` POD types + callback tables; domain modules enforce `#![deny(warnings)]` and `#![forbid(unsafe_code)]`, isolating pointer checks/`catch_unwind`/`*_free` in `ffi.rs`. `AddRustCrate.cmake` cross-compiles `arm64-v8a`, `x86_64`, `armeabi-v7a` using the NDK Clang wrapper, `CMAKE_AR`, and `-ldl -lm -llog`.

## ISPC Kernels & Runtime Switches

- **ISPC (`src/simd_kernels`, `AddIspcKernel.cmake`):** `ENABLE_ISPC_KERNELS` (`ON` non-Android, `OFF` Android); empty `INTERFACE` fallback when `ispc` is missing. Three kernels (`tex_etc2.ispc`, `tex_astc_scan.ispc`, `tex_swizzle.ispc`) compile multi-ISA (`sse4-i32x4,avx2-i32x8,avx512skx-i32x16` / `neon-i32x4`). Retirement requires 5 interleaved fresh-process runs (`EDEN_NX2_ISPC=0|1`) on x86-64 and ARM64 with bit-exact output and median C++ throughput within 5% (`[.astc_bench]`, `[.etc2_bench]`, `[.native_swizzle_bench]`).
- **Switches:** `EDEN_NX2_RUST_AMPERE`, `EDEN_NX2_DIRECT_SM86`, `EDEN_NX2_RUST_NUP`, `EDEN_NX2_RUST_SWITCH2_IO`, `EDEN_NX2_ISPC` (plus `*_VERIFY=1`), `EDEN_NX2_VK_VIDEO=1`, `EDEN_NX2_DRAW_CACHE`.
- **Verification:** `cargo test --manifest-path rust/Cargo.toml --locked --offline`; `cargo check --manifest-path rust/Cargo.toml --no-default-features --features ampere-spirv`; Catch2 `[ampere]`, `[nup]`, `[switch2]`, `[textures]`.
