# Historical CI, packaging and migration checklist

This checklist records the original rollout plan, when the standalone workspace contained only `eglantine_ampere_core`. Current target names and switches live in the [architecture ledger](../ARCHITECTURE.md#first-party-language-and-domain-ownership). Use [Phase 0](00_PREREQUISITES_AND_BASELINE.md) for fresh local evidence.

## Current entry points

| Area | Source and verification |
|---|---|
| Native build | The [root build](../CMakeLists.txt) and [Cargo helper](../CMakeModules/AddRustCrate.cmake) build and link the unified Rust archive. Confirm the target triple, linker and runtime in each supported toolchain configuration. |
| CI | The [Linux test workflow](../.github/workflows/test-linux.yml) runs Cargo tests. Check platform build and release jobs separately for packaging evidence. |
| Nix | The [package derivation](../nix/package.nix) declares Cargo build inputs, uses the lockfile and enables offline Cargo. Test the selected system and PGO variant when changing that wiring. |
| Source and release checks | The [architecture ledger](../ARCHITECTURE.md#source-and-release-checks) links the live workflow and tools. The [header checker tests](../tools/ci/test_license_header.py) cover accepted and missing SPDX headers for the source types considered in this proposal. |

The remaining sections preserve the proposed gates and original file matrix. They do not specify current dependencies or implementation work.

## 1. CI integration gates

| Workflow | Existing coverage | Evidence needed for integration |
|---|---|---|
| [Windows builds](../.github/workflows/build-windows.yml) | x86-64 MSVC and MinGW UCRT64 PGO builds | Link the Rust static library into each enabled executable using that job's target ABI, linker and CRT. Exercise both normal and PGO packages. A passing MSVC link covers only the MSVC job. |
| [Linux builds](../.github/workflows/build-linux.yml) | Native x86-64 and AArch64 jobs, with optional Clang PGO | Build for the runner's architecture. Verify the native static library link in each enabled configuration, including PGO. |
| [macOS build](../.github/workflows/build-macos.yml) | Apple Silicon app bundle and architecture checks | Link for `aarch64-apple-darwin` and verify the packaged application. Check runtime frameworks and entitlements when a later feature first needs them. |
| [Linux tests](../.github/workflows/test-linux.yml) | Script tests and a Catch2 job running CTest | Add `cargo test --manifest-path rust/Cargo.toml --locked` and run the differential harness. Once the emulator dispatch exists, run the Ampere tests with C++ selected and with Rust verification enabled. Confirm the Rust path actually ran. |
| [Nix builds](../.github/workflows/build-nix.yml) | Builds the flake package selected by its system and PGO matrix | Exercise the new toolchain inside the derivation. See the offline build checks below. |
| [Source checks](../.github/workflows/source-checks.yml) and [release packaging](../.github/workflows/release.yml) | Check tracked paths, PR license headers and release archive paths at their respective stages | Extend language coverage as needed. Inspect the packaged runtime dependencies and launch the built artifact on a matching host. |

Keep the existing C++ build usable with the new language disabled. A green Cargo test proves the crate builds and its tests pass. The emulator link, runtime selection and parity checks each need their own evidence before a default changes.

## 2. Nix packaging

The flake exposes `eglantine` and `eglantine-pgo` for x86-64 Linux, AArch64 Linux and AArch64 Darwin. Its default development shell is defined for Linux. [Flake outputs](../flake.nix) · [Development shell](../shell.nix)

1. The Phase 1A proposal added Cargo and rustc as native build tools. The lockfile originally contained only the prototype crate. If external crates are added, make their locked sources available as Nix build inputs before invoking Cargo. [Package derivation](../nix/package.nix) · [Cargo lockfile](../rust/Cargo.lock)
2. Wire the CMake target before enabling `EGLANTINE_RUST_OFFLINE`. The helper passes `--offline --frozen` to Cargo. Build with the Nix sandbox enabled to verify that Cargo needs no network during compilation. Nix may fetch declared inputs before the build. [CMake helper](../CMakeModules/AddRustCrate.cmake)
3. For the host system, run `nix build -L .#eglantine` and, when testing the PGO variant, `nix build -L .#eglantine-pgo`. Record the system and enabled feature for each build.
4. Add Slang or ISPC as host tools when their phases land. Keep runtime libraries in the target dependency set. If the same tool is added to the CPM manifest, make the Nix dependency resolver use the packaged tool. [CPM dependency resolver](../nix/build-eden-emulator.nix)

## 3. Source checks

The license script selects source files by extension. When introducing Rust, Slang, ISPC or Slint into the integrated build, add the relevant suffix to the script and cover both an accepted header and a missing header in its regression tests. The release-path checker scans path names. Inspect actual package contents and runtime dependencies separately. [License checker](../tools/ci/license-header.sh) · [License tests](../tools/ci/test_license_header.py) · [Release-path checker](../tools/ci/check_release_paths.py)

Record each implemented module's responsibility and fallback in the architecture ledger when it lands. [Architecture ledger](../ARCHITECTURE.md)

## 4. Original file matrix

The status entries apply to the original proposal. **DONE** identified a prototype file at that point. **NEW** and **EDIT** identified proposed work, and **VERIFY** identified behavior to exercise. Paths in this matrix include files that were proposed, renamed or removed. See the phase guides for current source links and verification.

### A. Root, Build Modules, Nix & CI
| Path | Phase | Status | Summary of Work |
|---|---|---|---|
| `POLYGLOT_IMPLEMENTATION_PLAN.md` | 0–4 | **[DONE]** | Master plan & links to `migration_docs/` |
| `migration_docs/00..06_*.md` | 0–4 | **[DONE]** | Detailed per-phase migration runbooks |
| `CMakeModules/AddRustCrate.cmake` | 1A | **[DONE / EDIT]** | Verify existing MinGW target selection and recursive source dependencies in the integrated build |
| `CMakeModules/AddIspcKernel.cmake` | 3A | **[NEW]** | Multi-ISA ISPC staticlib builder & header generator |
| `CMakeModules/CPMUtil.cmake` | 2A, 3A | **[EDIT]** | Support tool binaries `shader-slang` & `ispc` cleanly |
| `CMakeLists.txt` | 1A, 1B, 2A, 3A, 3D, 4A, 4B, 4C | **[EDIT]** | Add `ENABLE_RUST_*`, `ENABLE_ISPC_KERNELS`, `ENABLE_SLINT_SHELL`, `ENABLE_APPLE_HV`, wire crates |
| `src/CMakeLists.txt` | 3A, 4C | **[EDIT]** | Add `simd_kernels` & optional `shell_slint` |
| `cpmfile.json` | 2A, 3A | **[EDIT]** | Pin `shader-slang` & `ispc` hashes (`oaknut`/`zstd`/`sirit`/`fmt` already pinned) |
| `.github/workflows/{build-windows,build-linux,build-macos,build-nix,test-linux,ci,release}.yml` | 1A–4C | **[EDIT]** | Install/cache Rust, MSYS2 Rust for MinGW PGO, `cargo test`, offline Nix check |
| `flake.nix`, `nix/package.nix`, `nix/build-eden-emulator.nix`, `shell.nix` | 1A–4C | **[EDIT]** | Add phase-specific build inputs and exercise Cargo offline |
| `tools/ci/license-header.sh`, `tools/ci/test_license_header.py` | 1A | **[EDIT]** | Check SPDX headers in `.rs`, `.slang`, `.ispc` and `.slint` files |
| `tools/ci/check_release_paths.py`, `ARCHITECTURE.md` | 1A | **[VERIFY / EDIT]** | Verify no artifacts leak; record polyglot module DAG in `ARCHITECTURE.md` |

### B. Rust Workspace (`rust/`)
| Path | Phase | Status | Summary of Work |
|---|---|---|---|
| `rust/.gitignore`, `rust/Cargo.toml`, `rust/Cargo.lock` | 1A–4C | **[DONE / EDIT]** | Workspace root (`ampere_core` done; add `ampere_spirv`, `switch2_io`, `nup_fs`, `shell_slint_bridge`) |
| `rust/include/eglantine_ampere.h`, `rust/crates/ampere_core/**` | 1A | **[DONE]** | Std-only SM86->Maxwell lowering + capture + map + override + FFI |
| `rust/tools/diff_check/diff_check.cpp` | 1A | **[DONE]** | C++20 vs Rust bit-exactness CLI harness |
| `rust/include/eglantine_ampere_spirv.h`, `rust/crates/ampere_spirv/**` | 1B | **[NEW]** | Direct SM86 decode -> SSA IR -> SPIR-V 1.6 + native coop-matrix + `rayon` batch |
| `rust/include/eglantine_switch2_io.h`, `rust/crates/switch2_io/**` | 4A | **[NEW]** | Cross-platform BLE (`btleplug`) + USB (`nusb`) + Switch 2 protocol codecs |
| `rust/include/eglantine_nup_fs.h`, `rust/crates/nup_fs/**` | 4B | **[NEW]** | `memmap2` + `rayon` parallel `.nup` reader/writer linking C `zstd::zstd` |
| `rust/crates/shell_slint_bridge/**` | 4C | **[NEW]** | Slint UI screens (`.slint`) + state bridge crate |

### C. Shader Recompiler, Pipeline Cache, Host Shaders & VideoCore (`src/shader_recompiler/`, `src/video_core/`)
| Path | Phase | Status | Summary of Work |
|---|---|---|---|
| `src/shader_recompiler/CMakeLists.txt` | 1A, 1B | **[EDIT]** | Link `ampere_rs` (`1A`) & `ampere_spirv_rs` (`1B`), add `program_capture.h` |
| `src/shader_recompiler/frontend/ampere/{lower_to_maxwell,program_capture,program_address_map,program_override}.h` | 1A | **[EDIT]** | Add `EDEN_NX2_RUST_AMPERE` & `EDEN_NX2_RUST_AMPERE_VERIFY` FFI shims |
| `src/shader_recompiler/frontend/maxwell/translate/impl/internal_stage_buffer_entry_read.cpp` | 1A | **[VERIFY-ONLY]** | Keep `ISBERD` pseudo-op contract aligned for Maxwell fallback |
| `src/shader_recompiler/{shader_info,profile,runtime_info,program_header,stage}.h`, `backend/bindings.h` | 1B, 2B | **[EDIT / STATIC CHECK]** | Add layout/reflection `static_assert`s + calibrated `CoopHmmaProfile` fields |
| `src/shader_recompiler/backend/spirv/{emit_spirv,emit_spirv_tensor}.cpp` | 1B, 2B | **[EDIT]** | Parameterize HMMA lane permutation & tile size cleanly on fallback & direct paths |
| `src/video_core/CMakeLists.txt` | 2A, 2C, 2D, 3A, 3D | **[EDIT]** | Wire `vk_temporal_upscaler.h`, `vk_xess_upscaler.*`, `ispc`/`slang` hooks, ARM64 MME JIT |
| `src/video_core/shader_environment.{h,cpp}` | 1A, 1B | **[VERIFY / EDIT]** | `1A`: verify Rust parity; `1B`: add immutable `NativeShaderSnapshot` |
| `src/video_core/shader_cache.{h,cpp}`, `src/video_core/mkw_wave_workaround.h` | 1A, 1B | **[VERIFY / EDIT]** | Preserve negative rejection cache & MKW key `0x5a4f3726590efb17` workaround |
| `src/video_core/shader_warm/{warm_records,shader_warm_builder,shader_archives}.{h,cpp}` | 1A, 1B | **[VERIFY / EDIT]** | `1A`: verify; `1B`: batch-compile graphics+compute precache & bump versions |
| `src/video_core/renderer_vulkan/vk_pipeline_cache.{h,cpp}` | 1B, 2B | **[EDIT]** | Route `EDEN_NX2_DIRECT_SM86=1`, fallback cleanly, keep Vulkan and shader-warm cache versions synchronized when translation changes |
| `src/video_core/renderer_vulkan/vk_{compute,graphics}_pipeline.{h,cpp}` | 1B, 3B | **[EDIT]** | Direct prebuilt SPIR-V ingestion + descriptor hash caching |
| `src/video_core/renderer_vulkan/vk_cooperative_hmma_test.{h,cpp}` | 2B | **[EDIT]** | Upgrade binary test to `CoopHmmaProfile` tile & lane-map calibration |
| `src/video_core/vulkan_common/vulkan_{device,wrapper}.{h,cpp}` | 2B, 2D | **[EDIT]** | Query `CoopMatrixPropertiesKHR` + `VK_KHR_video_decode_queue` extensions/queues |
| `src/video_core/renderer_vulkan/vk_temporal_upscaler.h`, `vk_xess_upscaler.{h,cpp}` | 2C | **[NEW]** | `ITemporalUpscaler` interface & Intel XeSS runtime loader backend |
| `src/video_core/renderer_vulkan/{vk_streamline_dlss,vk_fsr3_upscaler,vk_dlss_network_probe,vk_texture_cache}.{h,cpp}`, `vulkan_common/streamline.{h,cpp}`, `src/common/settings*.h` | 2C | **[EDIT]** | Unify DLSS/XeSS/FSR3 under `ITemporalUpscaler` |
| `src/video_core/host_shaders/CMakeLists.txt`, `*.comp`, `hmma_tiled_fallback.slang`, `nv12_to_block_linear.slang` | 2A, 2D | **[EDIT / NEW]** | Compile `.slang` modules into `*_spv.h`; port hot texture/compute shaders incrementally |
| `src/video_core/host1x/nx2_video_decoder.{h,cpp}` (`codecs/*`, `nvdec*`, `vic*`) | 2D | **[EDIT / VERIFY]** | VRAM-resident Vulkan Video decode + FFmpeg fallback |
| `src/video_core/renderer_vulkan/{vk_descriptor_buffer,vk_update_descriptor,vk_rasterizer,vk_buffer_cache}.{h,cpp}`, `buffer_cache/buffer_cache_base.h` | 3B | **[EDIT]** | Persistent per-draw descriptor binding cache (`DescriptorBufferRing`) & download intervals |
| `src/video_core/renderer_vulkan/{vk_present_manager,vk_swapchain}.{h,cpp}` | 3C, 4C | **[EDIT]** | Verify `async_presentation` lock ordering, add VRR/120Hz pacing, host Quick Menu composite hook |
| `src/video_core/textures/{etc2,astc,bcn,decoders}.{h,cpp}`, `workers.{h,cpp}` | 3A | **[EDIT / VERIFY]** | Dispatch into ISPC SIMD kernels with `EDEN_NX2_ISPC` toggle & parity verify |
| `src/video_core/macro_turing_jit.h`, `macro.{h,cpp}`, `gpu_split.{h,cpp}`, `engines/{maxwell_3d,kepler_compute}.cpp` | 3B, 3D | **[EDIT]** | Split x64 (`xbyak`) / ARM64 (`oaknut`) MME JIT backends + benchmark `gpu_split` |
| `tools/nx2/lower_dump.cpp` | 1A, 1B | **[VERIFY / EXTEND]** | Keep header-only Phase 1A compilation intact; optional SPIR-V dump flag in 1B |

### D. Audio, Kernel, Services, ARM64 CPU, Input, FileSys & Shells
| Path | Phase | Status | Summary of Work |
|---|---|---|---|
| `src/audio_core/CMakeLists.txt`, `renderer/command/{mix/mix*,mix/volume*,resample/*,effect/*biquad*,data_source/adpcm}.{h,cpp}` | 3A | **[EDIT]** | Dispatch audio voice mixing/resampling/ADPCM/biquad loops to `ispc::*` |
| `src/simd_kernels/{CMakeLists.txt, *.ispc}` | 3A | **[NEW]** | Multi-ISA ISPC texture & audio SIMD kernels |
| `src/core/hle/kernel/{k_address_arbiter,k_condition_variable,k_scheduler,global_scheduler_context}.{h,cpp}`, `k_scheduler_lock.h`, `svc/svc_{address_arbiter,condition_variable}.cpp` | 3C | **[EDIT]** | Address-sharded waiter buckets & fast-path arbitration checks |
| `src/core/hle/service/{nx2_gsv.h, nx2_frame_interval.h}` | 2D, 3C | **[EDIT]** | `2D`: VRAM `WriteImage`; `3C`: non-blocking `vi2 229` flush + `FRAMES_AHEAD=1` fence fix |
| `src/core/CMakeLists.txt`, `src/core/arm/{arm_interface.{h,cpp}, hv/arm_hv.{h,cpp}}` | 3D, 4B | **[EDIT / NEW]** | Apple Silicon `Hypervisor.framework` backend + `nup_fs_rs` linkage |
| `src/input_common/CMakeLists.txt`, `helpers/switch2_protocol/{switch2_rust_transport.cpp, switch2_driver.*, switch2_types.*, switch2_ble_win.cpp, switch2_usb.cpp}`, `drivers/switch2*` | 4A | **[EDIT / NEW]** | Cross-platform BLE/USB (`switch2_io`) backend replacing Windows-only `windowsapp` dependency |
| `src/core/file_sys/{nup_container,nup_writer}.{h,cpp}`, `src/core/loader/nup.{h,cpp}`, `src/yuzu/nup_dialogs.{h,cpp}` | 4B | **[EDIT / VERIFY]** | Wire `nup_fs_rs` (`zstd::zstd` parity), verify `nup_dialogs` UI |
| `src/shell_slint/{CMakeLists.txt, main.cpp, emu_window_slint_vk.*, controller_bridge.*}` | 4C | **[NEW]** | Optional GPU-native 60/120Hz console shell (`eglantine-console`) |

### E. Verification & Test Suite Files (`src/tests/`)
| Path | Phase | Status | Summary of Work |
|---|---|---|---|
| `src/tests/CMakeLists.txt` | 1A–4B | **[EDIT]** | Register `ampere_rust_parity.cpp`, `ampere_direct_spirv.cpp`, `ispc_parity.cpp` |
| `src/tests/video_core/ampere_{lowering,compute_lowering,depth_compare,f2i,geometry,gradient,i2f,ldc,logged_numeric,returns,vote}.cpp` | 1A | **[VERIFY-ONLY]** | Run the full `[ampere]` group with C++ and integrated Rust and record the results |
| `src/tests/video_core/{ampere_rust_parity,ampere_direct_spirv}.cpp` | 1A, 1B | **[NEW]** | In-tree differential FFI parity tests & `spirv-val` validation tests |
| `src/tests/video_core/{shader_bench,spirv_software_matrix,native_macro_fifo,native_swizzle,nx2_perf*}.cpp` | 1B–3D | **[EDIT / VERIFY]** | Track shader compile speedups, coop-matrix profiles, draw-bind reductions, MME JIT parity |
| `src/tests/common/ispc_parity.cpp`, `src/tests/core/{nup_writer,nx2_frame_interval,nx2_gsv_bounds}.cpp`, `src/tests/input_common/{switch2_protocol,switch2_auto_pair}.cpp` | 3A–4B | **[NEW / VERIFY]** | SIMD texture/audio parity, NUP byte-identity, `vi2` frame pacing, `[switch2]` suite |
