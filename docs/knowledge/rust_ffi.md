# `rust_ffi` & `simd_kernels` — Ownership, Cargo Workspace, and ISPC

## First-party language and domain ownership

| Area | Entry points | Decision |
|---|---|---|
| Guest runtime & desktop host | `src/core`, `src/video_core`, `src/input_common`, `src/yuzu_cmd`, `rust/apps/eglantine_gpui` | C++ owns emulator state, the SDL host, and game discovery. GPUI (`eglantine`) owns the desktop shell and its library preferences. ARM assembly supports NCE and Dynarmic, and a C bridge handles macOS Dynarmic exceptions. |
| Guest shaders | `src/shader_recompiler`, `rust/crates/ampere_spirv` | C++ owns Ampere-to-Maxwell lowering, capture, address maps, and override codecs. Rust owns direct SM86 compute translation to SPIR-V (`EDEN_NX2_DIRECT_SM86`). |
| NUP archives | `src/core/file_sys/nup_container.cpp`, `src/core/file_sys/nup_writer.cpp` | C++ owns archive parsing, reading, writing, extraction, metadata, and journal recovery. |
| Switch 2 input | `src/input_common/helpers/switch2_protocol` | C++ owns packet codecs, USB/BLE transport, and controller lifecycle. |
| Host shaders & SIMD | `src/video_core/host_shaders`, `dist/post_shaders`, `src/simd_kernels` | Keep GLSL build + post shaders (Slang & ISPC audio retired). Keep 3 ISPC texture kernels until x86-64 + ARM64 throughput/parity gates pass. |
| Android frontend | `src/android/app`, `rust/apps/eglantine_android`, `src/common/android` | SDL3 + Rust (`eglantine_android`) + C++ host; Groovy Gradle packaging (zero Kotlin). |
| Build, CI, tools | CMake, Gradle, Cargo, `.github/`, `tools/` | Keep declarative build configs, Python release/CI checks, Perl DTrace/filters, and C crash helper. |

Retire a duplicate only after identical outputs, failure behavior, and acceptable performance (prefer C++ on a tie; verify desktop + ARM64 Android link before removing C++).

## Cargo Workspace (`rust/`, `std`-only, zero external `crates.io` deps in `rust/Cargo.lock`)

| Crate | Path | C ABI Header & Responsibility |
|---|---|---|
| `eglantine_ampere_spirv` | `rust/crates/ampere_spirv` | `rust/include/eglantine_ampere_spirv.h` — SM86->SSA IR->SPIR-V 1.6 emitter (`OpTypeCooperativeMatrixKHR`, `std::thread::scope`) |
| `eglantine_ds` | `rust/crates/ds_core` | Safe Rust API for NDS payload loading and ARM9/ARM7 execution |
| `eglantine_ctr` | `rust/crates/ctr_core` | Safe Rust API for restricted 3DSX loading and ARM11 execution |
| `eglantine_ffi` | `rust/crates/eglantine_ffi` | `staticlib` (`eglantine_rs`) exporting the direct SM86 `extern "C"` symbols via `#[used]` anchors |

- **Crate and CMake dependencies.** `eglantine_ffi` depends on `ampere_spirv` (`ampere-spirv`). `ENABLE_RUST_AMP_SPIRV` enables the feature when `CMakeLists.txt` builds `eglantine_rs` via `CMakeModules/AddRustCrate.cmake`.
- **C ABI & Android NDK:** Crossings use `#[repr(C)]` POD types + callback tables; domain modules enforce `#![deny(warnings)]` and `#![forbid(unsafe_code)]`, isolating pointer checks/`catch_unwind`/`*_free` in `ffi.rs`. `AddRustCrate.cmake` cross-compiles `arm64-v8a`, `x86_64`, `armeabi-v7a` using the NDK Clang wrapper, `CMAKE_AR`, and `-ldl -lm -llog`.

## Nintendo DS and Nintendo 3DS

Both dependency-free crates expose `Machine::load`, `step`, `run_until`, and read-only access to guest registers and memory. The headless entry points validate synthetic guest programs. Frontend launch and playable title support require later milestones.

- DS validates the ARM9 and ARM7 payload records and their referenced ranges in an NDS image. It models shared main RAM and private ARM7 RAM. A step runs ARM9 before ARM7. A guest fault stops both processors and preserves the effects of any instruction already retired.
- 3DS loads a restricted 3DSX image with zero relocation entries. It sets code, read-only data, and writable data permissions and zero-fills BSS. A step runs one ARM11 instruction. A fault leaves the failed instruction's state unchanged.
- Both instruction subsets cover unconditional ARM MOV immediate, LDR literal, STR immediate without writeback, and B.

Within each crate, `Cargo.toml` defines the package, `src/lib.rs` exports the public API, `src/loader.rs` validates images, `src/memory.rs` checks guest accesses, `src/cpu.rs` executes instructions, `src/machine.rs` owns session state and execution limits, `src/error.rs` defines typed faults, and `tests/boot.rs` verifies the public API with independently authored fixtures.

Run `cargo test --manifest-path rust/Cargo.toml --locked --offline -p eglantine_ds -p eglantine_ctr` for both cores, or drop the package selectors for the workspace.

## ISPC Kernels & Runtime Switches

- **ISPC (`src/simd_kernels`, `AddIspcKernel.cmake`):** `ENABLE_ISPC_KERNELS` (`ON` non-Android, `OFF` Android); empty `INTERFACE` fallback when `ispc` is missing. Three kernels (`tex_etc2.ispc`, `tex_astc_scan.ispc`, `tex_swizzle.ispc`) compile multi-ISA (`sse4-i32x4,avx2-i32x8,avx512skx-i32x16` / `neon-i32x4`). Retirement requires 5 interleaved fresh-process runs (`EDEN_NX2_ISPC=0|1`) on x86-64 and ARM64 with bit-exact output and median C++ throughput within 5% (`[.astc_bench]`, `[.etc2_bench]`, `[.native_swizzle_bench]`).
- **Switches.** `EDEN_NX2_DIRECT_SM86`, `EDEN_NX2_ISPC`, `EDEN_NX2_VK_VIDEO=1`, `EDEN_NX2_DRAW_CACHE`.
- **Verification:** `cargo test --manifest-path rust/Cargo.toml --locked --offline`; `cargo check --manifest-path rust/Cargo.toml --no-default-features --features ampere-spirv`; Catch2 `[ampere]`, `[nup]`, `[switch2]`, `[textures]`.
