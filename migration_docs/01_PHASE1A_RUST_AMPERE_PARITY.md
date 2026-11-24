# Phase 1A — Rust CMake Foundation & Ampere SM86 Lowering Parity (`ampere_core`)

> [!NOTE]
> **Integrated, runtime opt-in.** The [current build](../CMakeLists.txt) calls the [CMake helper](../CMakeModules/AddRustCrate.cmake) to link the unified Rust FFI archive. C++ remains the default. The [lowering entry point](../src/shader_recompiler/frontend/ampere/lower_to_maxwell.h) implements `EDEN_NX2_RUST_AMPERE` and `EDEN_NX2_RUST_AMPERE_VERIFY`. Run the [in-tree parity tests](../src/tests/video_core/ampere_rust_parity.cpp) with `[ampere]` and record the [Phase 0 baseline](00_PREREQUISITES_AND_BASELINE.md).
>
> The sections below preserve the original integration proposal. Use the [architecture ledger](../ARCHITECTURE.md#first-party-language-and-domain-ownership) for current target names, switches and parity requirements.

---

## 1. Standalone prototype

| Path | Role & Constraints | Key Public Symbols |
|---|---|---|
| [CMakeModules/AddRustCrate.cmake](../CMakeModules/AddRustCrate.cmake) | CMake helper `add_rust_static_library(...)`; creates IMPORTED global static lib, sets MSVC `/MD` vs `/MT`, sets `EGLANTINE_RUST_OFFLINE`, links Windows/macOS/Linux native std libs | `add_rust_static_library` |
| [rust/Cargo.toml](../rust/Cargo.toml) | Cargo workspace (`members = ["crates/ampere_core"]`), `[profile.release]` (`opt-level = 3`, `lto = "thin"`, `codegen-units = 1`; default unwind kept for `catch_unwind`) | — |
| [rust/.gitignore](../rust/.gitignore) | Excludes `/target/` and Rust backup files | — |
| [rust/Cargo.lock](../rust/Cargo.lock) | Workspace lockfile (zero external dependencies) | — |
| [rust/crates/ampere_core/Cargo.toml](../rust/crates/ampere_core/Cargo.toml) | Package `eglantine_ampere_core`, lib `eglantine_ampere_rs` (`["rlib", "staticlib"]`), std-only, 0 external deps | — |
| [rust/include/eglantine_ampere.h](../rust/include/eglantine_ampere.h) | C99/C++20 `extern "C"` ABI header (`EglAmpereInstruction`, `EglAmpereCaptureState`, `EglAmpereLoweringOptions`, `EglAmpereLoweredProgram`, `EglAmpereBytes`) | All 11 `egl_ampere_*` C functions |
| [rust/crates/ampere_core/src/lib.rs](../rust/crates/ampere_core/src/lib.rs) | Crate root (`#![deny(warnings)]`), exports modules & re-exports Rust API | `lower_to_maxwell`, `ProgramCaptureState`, etc. |
| [rust/crates/ampere_core/src/types.rs](../rust/crates/ampere_core/src/types.rs) | `#![forbid(unsafe_code)]`; `Instruction`, `LoweredProgram`, `LoweringOptions`, `TensorModeOverride`, `native_register_copy`, env helpers (`EDEN_NX2_LOWER_EXTENDED`, `EDEN_NX2_TENSOR`) | `Instruction::bits`, `LoweredProgram::is_ok` |
| [rust/crates/ampere_core/src/capture.rs](../rust/crates/ampere_core/src/capture.rs) | `#![forbid(unsafe_code)]`; 1:1 port of `Shader::Ampere::ProgramCaptureState` | `ProgramCaptureState`, `CaptureResult` |
| [rust/crates/ampere_core/src/address_map.rs](../rust/crates/ampere_core/src/address_map.rs) | `#![forbid(unsafe_code)]`; 1:1 port of `program_address_map.h` (`0x50414d4e4544454e` magic) | `has_native_address_map`, `append_native_address_map`, `native_program_address` |
| [rust/crates/ampere_core/src/override_codec.rs](../rust/crates/ampere_core/src/override_codec.rs) | `#![forbid(unsafe_code)]`; 1:1 port of `program_override.h` (FNV-1a key + `EDEN`/`OV02` override container) | `native_program_key`, `encode_program_override`, `decode_program_override`, `program_override_source_size` |
| [rust/crates/ampere_core/src/pressure.rs](../rust/crates/ampere_core/src/pressure.rs) | `#![forbid(unsafe_code)]`; 1:1 port of `CollectRegisterOperands` & `PrepareRegisterPressure` (`R216`-`R254` aliasing) | `RegisterPressure`, `collect_register_operands`, `prepare_register_pressure` |
| [rust/crates/ampere_core/src/lower/mod.rs](../rust/crates/ampere_core/src/lower/mod.rs) | `#![forbid(unsafe_code)]`; `LowerCtx`, uniform mirror retry loop (`UR28+`), relocation patching, dispatch across 5 submodules | `lower_to_maxwell`, `lower_to_maxwell_with` |
| [rust/crates/ampere_core/src/lower/fp_half.rs](../rust/crates/ampere_core/src/lower/fp_half.rs) | `#![forbid(unsafe_code)]`; FP32, FP16x2, F2F/F2FP, F2I/F2IP, I2F/I2FP, MUFU, FSET/FSETP/ISETP/HSET2/HSETP2 | `try_lower` |
| [rust/crates/ampere_core/src/lower/int_logic.rs](../rust/crates/ampere_core/src/lower/int_logic.rs) | `#![forbid(unsafe_code)]`; IMAD/IMUL/ISCADD/IADD3/LEA/LOP3/SHF/PRMT/FLO/POPC/VOTE/SHFL/PSETP/uniform ops | `try_lower` |
| [rust/crates/ampere_core/src/lower/memory_tex.rs](../rust/crates/ampere_core/src/lower/memory_tex.rs) | `#![forbid(unsafe_code)]`; LDC/ULDC, LDG/STG/ATOM/RED, LDL/STL/LDS/STS, TEX/TLD/TLD4/TXQ/TMML, SULD/SUST/SURED | `try_lower` |
| [rust/crates/ampere_core/src/lower/tensor.rs](../rust/crates/ampere_core/src/lower/tensor.rs) | `#![forbid(unsafe_code)]`; `MOVM` (`0x23a`), `HMMA` (`0x23c` disabled/inline/compact/stub), `LDSM` (`0x83b`) | `try_lower` |
| [rust/crates/ampere_core/src/lower/control_flow.rs](../rust/crates/ampere_core/src/lower/control_flow.rs) | `#![forbid(unsafe_code)]`; `EXIT` (`0x94d`), `BRA` (`0x947`), `CALL` (`0x944`), `RET` (`0x950`), `BRX` (`0x949`/`0x958`), `LEPC` (`0x34e`), `BAR`/`ERRBAR`/`DEPBAR`/`MEMBAR` | `try_lower` |
| [rust/crates/ampere_core/src/ffi.rs](../rust/crates/ampere_core/src/ffi.rs) | Only file allowed `unsafe`; validates pointers/alignment, `catch_unwind`, `Box::into_raw` / idempotent free | All 11 `egl_ampere_*` extern symbols |
| [rust/crates/ampere_core/tests/parity_tests.rs](../rust/crates/ampere_core/tests/parity_tests.rs) | 7 Rust integration test suites (capture, address map, override codec, pressure, lowering, tensor/UR28, FFI safety) | — |
| [rust/tools/diff_check/diff_check.cpp](../rust/tools/diff_check/diff_check.cpp) | Standalone C++20 differential harness comparing `Shader::Ampere::*` vs `egl_ampere_*` (the original prototype run reported 6,200 cases and 0 mismatches) | — |

---

## 2. Integrate the prototype into CMake

### 2.1 Check the Cargo build helper

The helper already accepts `EGLANTINE_RUST_TARGET`, chooses an archive per CMake configuration, and uses `GLOB_RECURSE` for Rust sources. Verify these behaviors with the generator and toolchain used by each build job.

- On UCRT64, compare the Cargo host from `rustc -vV` with CMake's selected architecture and linker. Link and run a consumer of the archive to check the target and C runtime pairing.
- In a scratch build, change a nested source under `src/lower/` and confirm Ninja rebuilds the Rust archive and relinks the consumer.
- Build Debug and Release consumers. Check the imported archive path and the MSVC runtime for each configuration.

### 2.2 Add the opt-in target

Define the option in the root CMake file and create the target after the toolchain architecture is known and before adding the shader recompiler. The helper discovers Cargo when invoked. Keep room-only builds and the default build independent of Rust.

```cmake
option(ENABLE_RUST_AMPERE "Build the Rust Ampere lowering core" OFF)
if (ENABLE_RUST_AMPERE AND NOT YUZU_STATIC_ROOM)
    include(AddRustCrate)
    add_rust_static_library(
        TARGET ampere_rs
        MANIFEST "${CMAKE_SOURCE_DIR}/rust/Cargo.toml"
        PACKAGE eglantine_ampere_core
        LIB_NAME eglantine_ampere_rs
        INCLUDE_DIR "${CMAKE_SOURCE_DIR}/rust/include"
    )
endif()
```

### 2.3 [EDIT] `src/shader_recompiler/CMakeLists.txt`
- **Lines 66-68:** Also add `frontend/ampere/program_capture.h` alongside `lower_to_maxwell.h`, `program_override.h`, and `program_address_map.h` so IDE targets index it cleanly.
- **Line 259 (`target_link_libraries`):**
  ```cmake
  if (TARGET ampere_rs)
      target_link_libraries(shader_recompiler PUBLIC ampere_rs)
      target_compile_definitions(shader_recompiler PUBLIC EGLANTINE_ENABLE_RUST_AMPERE=1)
  endif()
  ```
  *(Using `PUBLIC` ensures `video_core`, `tests`, `yuzu`, and `yuzu-cmd` automatically inherit `rust/include` for `"eglantine_ampere.h"` and link Windows/Linux/macOS native libs through `ampere_rs`.)*

---

## 3. Step-by-Step C++ Bridge Edits (Feature-Gated Differential Rollout)

All C++ Ampere frontend files remain header-only (`inline` functions) so header-only tools like [tools/nx2/lower_dump.cpp](../tools/nx2/lower_dump.cpp) keep compiling standalone with the project's C++20 and fmt settings when `EGLANTINE_ENABLE_RUST_AMPERE` is NOT defined!

### 3.1 [EDIT] `src/shader_recompiler/frontend/ampere/lower_to_maxwell.h`
1. Guarded include at top of file (after `#include "common/common_types.h"`, L20):
   ```cpp
   #ifdef EGLANTINE_ENABLE_RUST_AMPERE
   #include "eglantine_ampere.h"
   #endif
   ```
2. Inside `namespace Shader::Ampere` near `LowerToMaxwell(...)` (bottom of file, ~L3995-4004):
   - Preserve C++ `LowerToMaxwell` renamed/factored as `inline LoweredProgram LowerToMaxwellCpp(std::span<const Instruction> code, LoweringOptions options = {})`.
   - When `EGLANTINE_ENABLE_RUST_AMPERE` is defined:
     - Read two runtime env switches (cached or per-call cleanly):
       - `EDEN_NX2_RUST_AMPERE`: `"1"` routes `LowerToMaxwell` to `LowerToMaxwellRust`.
       - `EDEN_NX2_RUST_AMPERE_VERIFY`: `"1"` runs **both** `LowerToMaxwellCpp` and `LowerToMaxwellRust`, compares every word of `instructions`, `source_instruction_offsets`, `source_size`, `required_texture_buffer`, `high_uniform_registers`, `error_offset`, and `error`, and records the program key and the first differing field. Verification returns the C++ result and takes precedence over Rust selection. Make a mismatch fail the parity test.
     - Implement RAII wrapper around `EglAmpereLoweredProgram` calling `egl_ampere_free_lowered(&raw)` in destructor so exception safety holds across `Shader::RuntimeError` paths.
     - Copy outputs cleanly into `Shader::Ampere::LoweredProgram` only after checking the ABI return status. An ABI error and a successful call reporting a shader error are distinct. Read a pointer only when its corresponding count is nonzero.
   - **Important detail on `EDEN_NX2_TENSOR`:** Keep `lower_extended_override = -1` in `EglAmpereLoweringOptions` so Rust reads `EDEN_NX2_LOWER_EXTENDED` dynamically just like C++. Keep `EDEN_NX2_TENSOR` consistent between both implementations (already cached identically via env string `"1"`, `"compact"`, `"stub"`).

### 3.2 [EDIT] `src/shader_recompiler/frontend/ampere/program_capture.h`
- Currently lines 11-57 (`class ProgramCaptureState`).
- Add `#ifdef EGLANTINE_ENABLE_RUST_AMPERE` differential hook inside `Observe(const Instruction& instruction, u32 index)`:
  - When `EDEN_NX2_RUST_AMPERE=1` (or `EDEN_NX2_RUST_AMPERE_VERIFY=1`), maintain `EglAmpereCaptureState rs_state` initialized in constructor via `egl_ampere_capture_init(&rs_state)`, call `egl_ampere_capture_observe`, and verify `Result` matches C++.

### 3.3 [EDIT] `src/shader_recompiler/frontend/ampere/program_address_map.h`
- Lines 13-32 (`NativeAddressMapMagic`, `HasNativeAddressMap`, `AppendNativeAddressMap`, `NativeProgramAddress`).
- Add optional `EDEN_NX2_RUST_AMPERE` / `EDEN_NX2_RUST_AMPERE_VERIFY` dispatch inside `HasNativeAddressMap` and `NativeProgramAddress` calling `egl_ampere_has_native_address_map` / `egl_ampere_native_program_address`. (Note `AppendNativeAddressMap` pushes onto `std::vector<u64>` cleanly in 5 lines — keep C++ allocation wrapper.)

### 3.4 [EDIT] `src/shader_recompiler/frontend/ampere/program_override.h`
- Lines 13-88 (`NativeProgramKey`, `EncodeProgramOverride`, `ProgramOverrideSourceSize`, `DecodeProgramOverride`).
- Add optional `EDEN_NX2_RUST_AMPERE` / `EDEN_NX2_RUST_AMPERE_VERIFY` paths calling:
  - `egl_ampere_native_program_key`
  - `egl_ampere_encode_override` + `egl_ampere_free_bytes`
  - `egl_ampere_override_source_size`
  - `egl_ampere_decode_override` + `egl_ampere_free_lowered`
- Note: [ffi.rs](../rust/crates/ampere_core/src/ffi.rs) line 297 already accepts non-null `prog.error` when `prog.error_len == 0` (matching MSVC `std::string{}.data()`).

---

## 4. Callers and parity coverage

Every file in the repository referencing `frontend/ampere/*` or `Shader::Ampere::*` was scanned. None require breaking API edits in Phase 1A because `Shader::Ampere::*` signatures stay identical:

| File | Current Usage | Action in Phase 1A |
|---|---|---|
| `src/shader_recompiler/CMakeLists.txt` | Lists 3 of 4 headers at L66-68; links `common`, `fmt`, `sirit` | **[EDIT]** Link `ampere_rs`, define `EGLANTINE_ENABLE_RUST_AMPERE`, add `program_capture.h` |
| `src/shader_recompiler/frontend/ampere/{lower_to_maxwell,program_capture,program_address_map,program_override}.h` | Reference C++ implementation | **[EDIT]** Add gated `EDEN_NX2_RUST_AMPERE` / `_VERIFY` FFI shims |
| `src/video_core/shader_environment.h` (L35-38) | Forward-declares `Instruction`, `LoweredProgram` | **[VERIFY-ONLY]** Unchanged |
| `src/video_core/shader_environment.cpp` (L77, L241, L536-665, L1160) | Calls `ProgramCaptureState`, `LowerToMaxwell`, `NativeProgramKey`, `ProgramOverrideSourceSize`, `DecodeProgramOverride`, `AppendNativeAddressMap`, `NativeProgramAddress` | **[VERIFY-ONLY]** Automatically exercises Rust path under `EDEN_NX2_RUST_AMPERE=1` |
| `src/video_core/shader_warm/warm_records.cpp` (L10-96) | Calls `ProgramCaptureState::Observe` + `LowerToMaxwell` for compute precaching | **[VERIFY-ONLY]** Automatically tested via `shader_precache.cpp` (`[video_core]`) & Switch 2 warm boot |
| `src/video_core/mkw_wave_workaround.h` (L10-25) | Calls `NativeProgramKey` to patch key `0x5a4f3726590efb17` | **[VERIFY-ONLY]** Unchanged; key parity already guaranteed |
| `src/shader_recompiler/frontend/maxwell/translate/impl/internal_stage_buffer_entry_read.cpp` | Maxwell IR translator for `ISBERD` pseudo-op emitted by Ampere lowering | **[VERIFY-ONLY]** Does not call `Shader::Ampere` directly; keep pseudo-encoding aligned until Phase 1B bypasses it |
| `tools/nx2/lower_dump.cpp` | Standalone header-only dump triage CLI (requires C++20 and fmt) | **[VERIFY-ONLY]** Stays buildable without Rust because `#ifdef EGLANTINE_ENABLE_RUST_AMPERE` is opt-in |
| `src/tests/video_core/ampere_{lowering,compute_lowering,depth_compare,f2i,geometry,gradient,i2f,ldc,logged_numeric,returns,vote}.cpp` (11 files) | Existing Catch2 coverage | **[VERIFY-ONLY]** Run twice: default C++ and `EDEN_NX2_RUST_AMPERE=1 EDEN_NX2_RUST_AMPERE_VERIFY=1` |
| `src/tests/video_core/shader_bench.cpp` & `spirv_software_matrix.cpp` | Compile benchmark & `LowerToMaxwellWith` HMMA lowering test | **[VERIFY-ONLY]** Retain as reference coverage. Tests calling `LowerToMaxwellWith` directly bypass the proposed top-level dispatch. |
| `src/tests/CMakeLists.txt` + `src/tests/video_core/ampere_rust_parity.cpp` | Catch2 parity test inside main test binary | **[NEW / EDIT]** Add Catch2 test running `EDEN_NX2_RUST_AMPERE_VERIFY` assertions directly inside `tests.exe "[ampere]"` |

---

## 5. Verify and roll back

### Standalone checks from the original rollout

From the repository root, use Cargo's locked workspace to test and build the prototype.

```powershell
cargo test --locked --manifest-path rust/Cargo.toml
cargo build --locked --release --manifest-path rust/Cargo.toml
```

Cargo builds the library only. Rebuild the differential executable against that library before recording results. The current harness uses Windows APIs. In an x64 MSVC developer PowerShell with a matching native Rust toolchain, default Cargo target directory, and a configured CMake build using bundled fmt, ask rustc for its native link dependencies and pass them to the C++ linker.

```powershell
$linkInfo = cargo rustc --locked --release --manifest-path rust/Cargo.toml -p eglantine_ampere_core -- --print native-static-libs 2>&1
if ($LASTEXITCODE -ne 0) { throw ($linkInfo -join [Environment]::NewLine) }
$line = $linkInfo | Select-String 'native-static-libs:\s+(.+)$' | Select-Object -Last 1
if (-not $line) { throw 'rustc did not report native link libraries' }
$nativeLibs = $line.Matches[0].Groups[1].Value.Trim() -split '\s+'
$fmt = Select-String -Path build/CMakeCache.txt -Pattern '^FMT_SOURCE_DIR:[^=]+=(.+)$'
if (-not $fmt) { throw 'Set the include path to the fmt headers used by your CMake build' }
$fmtInclude = Join-Path $fmt.Matches[0].Groups[1].Value 'include'
cl /I $fmtInclude /DFMT_HEADER_ONLY /utf-8 /nologo /std:c++20 /EHsc /O2 /MD /I src /I rust/include rust/tools/diff_check/diff_check.cpp /Forust/target/diff_check.obj /Ferust/target/diff_check.exe /link rust/target/release/eglantine_ampere_rs.lib $nativeLibs
if ($LASTEXITCODE -ne 0) { throw 'Differential harness build failed' }
.\rust\target\diff_check.exe
if ($LASTEXITCODE -ne 0) { throw 'C++ and Rust differ' }
```

Record the case and mismatch counts with the compiler versions. A passing harness covers its corpus. It does not exercise capture and lowering through an emulator frontend.

### Proposed emulator rollout gates

1. Configure one build with `-DENABLE_RUST_AMPERE=OFF` and one with `-DENABLE_RUST_AMPERE=ON`, using the same project options. In each build run `cmake --build <build-dir> --target yuzu yuzu-cmd tests --parallel` with the actual directory substituted.
2. Run the built tests with `"[ampere]"` and `"[video_core]"` in the C++ build. Repeat in the enabled build in three fresh processes per suite: both switches unset, verification only, then Rust only. Record the selected implementation and comparison count. Verification must exercise both implementations and report zero differences.
3. Repeat the Phase 0 gameplay checkpoints in Rust-only and verification modes. Retain the C++ default until ABI, shader output, failure paths and frontend behavior pass on the supported toolchains. Use the [CI checklist](06_BUILD_CI_PACKAGING_AND_CHECKLIST.md) to track outstanding platforms.
4. To fall back in an integrated build, restart with both runtime switches disabled. For a toolchain or link failure, configure the C++-only build with `-DENABLE_RUST_AMPERE=OFF` and rerun the baseline.
