# Phase 1B — Direct SM86 -> SSA IR -> SPIR-V 1.6 Compiler (`ampere_spirv`)

> [!NOTE]
> **Proposed after Phase 1A.** The current [workspace manifest](../rust/Cargo.toml) contains only the Ampere lowering crate. There is no direct compiler or runtime selector in the emulator. The design below targets SPIR-V 1.6 on devices whose Vulkan profile supports it.
>
> The current translator can read live GPU state while compiling. Capture all inputs on the owning thread before queuing a worker. Keep the Maxwell path available when an input cannot be captured or a device lacks a required capability. See the [environment interface](../src/video_core/shader_environment.h) and the [Phase 1A parity gates](01_PHASE1A_RUST_AMPERE_PARITY.md).

---

## 1. Architecture: Direct Pipeline & Fallback Coexistence

```
Guest SM86 Program + SPH Header + GPU Snapshot
       │
       ├──► (EDEN_NX2_DIRECT_SM86=1) ──► Rust `ampere_spirv` (SM86 Decode -> SSA IR -> SPIR-V 1.6 + Reflection)
       │         │
       │         ├── Ok ──► SpirvModule bytes + Shader::Info metadata ──► vkCreateShaderModule
       │         └── Unsupported opcode / fallback ──┐
       │                                             ▼
       └──► (Fallback / Default) ──────► Current SM86->Maxwell lowering -> Maxwell Translator -> Yuzu IR -> Sirit SPIR-V
```

- **Important Invariant:** Both paths share [Shader::Backend::Bindings](../src/shader_recompiler/backend/bindings.h) descriptor set/binding numbering and Switch 2 workarounds (`mkw_wave_workaround`, blit cbuf 17 synthesis, `ISBERD`/`ISBEWR` task+mesh stage buffers) so pipelines behave identically on Vulkan.

---

## 2. New Rust Crate & C ABI (`rust/crates/ampere_spirv/`)

### 2.1 `[NEW]` Rust Workspace Expansion
- **[EDIT] `rust/Cargo.toml`**: Add `"crates/ampere_spirv"` to `members`. Add workspace dependency `rayon` (pinned in `rust/Cargo.lock` and vendored for offline Nix builds). Avoid heavy proc-macro dependency trees where a clean hand-rolled SPIR-V 1.6 word builder (`Vec<u32>`) suffices (`0` C/C++ deps!).
- **[NEW] `rust/crates/ampere_spirv/Cargo.toml`**: Package `eglantine_ampere_spirv`, lib `eglantine_ampere_spirv` (`["rlib", "staticlib"]`), depends on `ampere_core` (for `Instruction`, `ProgramCaptureState`, `native_program_key`, opcode/register metadata) + `rayon`.
- **[NEW] `rust/crates/ampere_spirv/src/`**:
  - `lib.rs` — `#![deny(warnings)]`, exposes `compile_shader(&ShaderSnapshot) -> CompileOutput`, `compile_batch(&[ShaderSnapshot]) -> Vec<CompileOutput>`.
  - `decode_sm86.rs` — Decodes SM86 control/predicate/operand/modifier fields directly (informed by Mesa NAK SM80/SM86 opcode tables cleanly implemented from scratch).
  - `ssa_ir.rs` — Compact arena-allocated SSA IR (basic blocks, structured control flow via dominators/post-order, copy propagation, DCE, constant folding, algebraic simplification, uniform-to-push/cbuf scalarization).
  - `spirv_emit.rs` — Direct SPIR-V 1.6 emitter:
    - Logical addressing + Vulkan Memory Model (`SPV_KHR_vulkan_memory_model`) when needed.
    - Native `OpTypeCooperativeMatrixKHR`, `OpCooperativeMatrixLoadKHR`, `OpCooperativeMatrixStoreKHR`, and `OpCooperativeMatrixMulAddKHR` (`SPV_KHR_cooperative_matrix`, plus optional `SPV_NV_cooperative_matrix2` tensor addressing) consuming calibrated lane layouts from Phase 2B!
    - Mesh/Task shader stages (`VK_EXT_mesh_shader`, `EmitMeshTasksEXT`, `SetMeshOutputsEXT`) handling `ISBERD`/`ISBEWR` directly without emitting temporary stage-buffer hacks.
    - Blit bank-24 (`17`) constant synthesis and MKW wave workaround (`0x5a4f3726590efb17`) parity.
  - `reflect.rs` — Populates C ABI reflection struct matching `Shader::Info` (`uses_fp16`, `uses_int64`, `uses_cooperative_hmma`, descriptor binding bitmasks, cbuf/SSBO/texture/image descriptors).
  - `ffi.rs` — `catch_unwind`-guarded `extern "C"` entrypoint.

### 2.2 `[NEW]` `rust/include/eglantine_ampere_spirv.h`
Define flat POD ABI types (mirrors `EglAmpereLoweredProgram` lifecycle patterns):
- `EglSpirvCompileInput`:
  - `stage` (Vertex/TessCtrl/TessEval/Geometry/Fragment/Compute/Task/Mesh), `program_header[10]` (`u64`), SM86 `const EglAmpereInstruction* code` + `size_t count`, `program_key` (`u64`), `workgroup_size[3]`, `local_memory_size`, `shared_memory_size`, `is_task_shader`, `is_mesh_shader`.
  - Give each snapshot slice its own element count. Keep backing storage alive until compilation completes, and copy any input retained by a worker.
  - Pre-captured snapshot slices (`const EglCbufEntry* cbufs`, `const EglTextureInfo* textures`, `const EglStoragePtrInfo* storage_ptrs`, `const EglCbufReplace* replacements`).
  - `EglSpirvProfile`: host subgroup/float16/int64/storage flags + `EglCoopMatrixLayout` (A/B/C/D element swap/stride from Phase 2B probe).
  - `EglBindingsBase`: initial binding offsets from `Shader::Backend::Bindings`.
- `EglSpirvCompileOutput`:
  - `uint32_t* spirv_words`, `size_t word_count`
  - `uint8_t fallback_to_maxwell` (1 if an unimplemented SM86 form asks C++ lowering to handle it)
  - Reflected `EglShaderInfoSummary` (cbuf descriptors, SSBO descriptors, sampled textures, storage images, push constants, `uses_fp16`, `requires_full_subgroups`, etc.)
  - `char* error`, `size_t error_len`
- Entrypoints:
  - `egl_ampere_spirv_compile(const EglSpirvCompileInput*, EglSpirvCompileOutput*)`
  - `egl_ampere_spirv_compile_batch(const EglSpirvCompileInput* inputs, size_t count, EglSpirvCompileOutput* outputs)` (parallelized internally via `rayon`)
  - `egl_ampere_spirv_free(EglSpirvCompileOutput*)`

---

## 3. Exact C++ Files to Create or Modify

| File | Kind | Exact Responsibilities & Edits Required |
|---|---|---|
| `CMakeLists.txt` | **[EDIT]** | Add `option(ENABLE_RUST_AMP_SPIRV "Build Rust direct SM86->SPIR-V compiler" OFF)` and call `add_rust_static_library(TARGET ampere_spirv_rs MANIFEST "${CMAKE_SOURCE_DIR}/rust/Cargo.toml" PACKAGE eglantine_ampere_spirv LIB_NAME eglantine_ampere_spirv INCLUDE_DIR "${CMAKE_SOURCE_DIR}/rust/include")` (respecting `EGLANTINE_RUST_OFFLINE`). |
| `src/shader_recompiler/CMakeLists.txt` | **[EDIT]** | Link `ampere_spirv_rs` to `shader_recompiler` (`PUBLIC`) and define `EGLANTINE_ENABLE_RUST_AMP_SPIRV=1`. |
| `src/video_core/shader_environment.h` | **[EDIT]** | Add `struct NativeShaderSnapshot` right alongside `LoweredNativeCode` (L55-68) storing immutable captured SM86 words + SPH header + copied `texture_types`, `texture_pixel_formats`, `cbuf_values`, `storage_pointer_types`, `cbuf_replacements`, `local_memory_size`, `shared_memory_size`, `workgroup_size` so compilation never touches live `Tegra::MemoryManager` or engine state on worker threads. Add `BuildNativeSnapshot()` declaration to `GenericEnvironment` (`L69-180`). |
| `src/video_core/shader_environment.cpp` | **[EDIT]** | Implement `BuildNativeSnapshot()` inside `LowerNativeCode()` (L532-685) capturing initial SM86 program bytes, `ProgramHeader`, and pre-probing referenced cbufs/textures/storage pointers on the calling thread, producing a self-contained snapshot safe for async `ThreadPool` or Rust `rayon` batches. |
| `src/video_core/shader_cache.h` / `shader_cache.cpp` | **[EDIT]** | If caching a result, include the stage, captured inputs, effective host profile, runtime specialization, compiler version and binding base in its identity. A native program key alone cannot distinguish different captured values or device capabilities. |
| `src/shader_recompiler/shader_info.h`, `profile.h`, `runtime_info.h`, `program_header.h`, `stage.h`, `backend/bindings.h` | **[VERIFY-ONLY / STATIC CHECK]** | Add `static_assert` checks or helper converters bridging `Shader::Profile`, `Shader::RuntimeInfo`, `Shader::Backend::Bindings`, and `Shader::Info` <-> `EglSpirv*` POD structs cleanly (must fail compile if any C++ struct field shifts without updating `eglantine_ampere_spirv.h`). |
| `src/shader_recompiler/backend/spirv/emit_spirv.cpp`, `src/shader_recompiler/backend/spirv/emit_spirv_tensor.cpp` | **[EDIT (Later Optional Simplification)]** | Keep existing `DefineCooperativeHmmaTypes`/`EmitCooperativeHmma`/`PatchCooperativeHmma` (L32-99) for Maxwell-IR fallback builds; add helper or shared tile-mapping table consumed by both direct SPIR-V and Sirit fallback (see Phase 2B). |
| `src/video_core/renderer_vulkan/vk_pipeline_cache.h` & `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp` | **[EDIT]** | 1. Guard direct compiler behind `EDEN_NX2_DIRECT_SM86` (default `"0"` initially, later `"1"`; `EDEN_NX2_DIRECT_SM86_VERIFY=1` runs `spirv-val` / compares descriptor layouts & HMMA behavior).<br>2. Add `TryCompileDirectSm86(...)` in `CreateComputePipeline` (L3460) and `CreateGraphicsPipeline` (L2469); on `fallback_to_maxwell == 1`, fall through seamlessly to Maxwell lowering + `TranslateProgram`.<br>3. When default changes or output format shifts, **bump `CACHE_VERSION`** in lockstep with `VideoCommon::ShaderWarm::WarmCacheVersion` (`static_assert` at `L94`). |
| `src/video_core/renderer_vulkan/vk_compute_pipeline.h` / `.cpp` & `vk_graphics_pipeline.h` / `.cpp` | **[EDIT]** | Allow pipeline creation directly from prebuilt `std::vector<u32>` SPIR-V + reflected `Shader::Info` without allocating `ShaderPools` (`vk_pipeline_cache.h` L109) on hits to the direct path. |
| `src/video_core/shader_warm/warm_records.h`, `warm_records.cpp`, `shader_warm_builder.h`, `shader_warm_builder.cpp`, `shader_archives.h`, `shader_archives.cpp` | **[EDIT]** | 1. Keep `WarmCacheVersion` (`warm_records.h` L24) equal to `CACHE_VERSION` (`vk_pipeline_cache.cpp` L93) and bump `WarmBuilderVersion`.<br>2. Replace per-shader single-threaded/small-pool C++ lowering loops in `BuildWarmFilesWith` (`shader_warm_builder.cpp`) with batched calls to `egl_ampere_spirv_compile_batch` (or `egl_ampere_lower_to_maxwell` + Maxwell fallback), expanding pre-warming from compute-only (`warm-compute.bin`) to **graphics + compute** snapshots extracted from game `.nup` shader archives (`shader_archives.cpp`). |
| `src/tests/CMakeLists.txt` & **[NEW] `src/tests/video_core/ampere_direct_spirv.cpp`** | **[EDIT / NEW]** | Validate generated SPIR-V 1.6 words with `spvtools::SpirvTools(SPV_ENV_VULKAN_1_3).Validate(...)`, verify descriptor binding parity against `Shader::Maxwell -> Sirit` reference on representative shaders, and run [src/tests/video_core/spirv_software_matrix.cpp](../src/tests/video_core/spirv_software_matrix.cpp) + [src/tests/video_core/shader_bench.cpp](../src/tests/video_core/shader_bench.cpp) tracking compile speedups. |

---

## 4. Verification and fallback gates

1. **Capture and replay.** Capture a representative graphics and compute program on the GPU thread, then compile each on a worker after the live environment is gone. Replay must produce the same module and reflection from identical inputs. Exercise a missing lookup, an unsupported opcode and a device profile below the required SPIR-V version. Each must select the reference compiler before installing a direct pipeline.
2. **Validate the module and its bindings.** Validate emitted SPIR-V 1.6 modules with `spirv-val --target-env vulkan1.3 <module.spv>`, substituting the output path. Compare reflection, binding allocation across stages, workgroup size and required capabilities with the reference path. Validate against the actual device profile before enabling a direct shader on it.
3. **Compare rendering.** Start from the same scene, settings and shader inputs in separate reference and direct runs. Compare the Phase 0 captures, including MKW water, 3D LUT tonemapping, ASTC UI, mesh/task geometry and compute HMMA. Record fallback counts and reasons alongside successful direct compiles. A scene rendered entirely through fallback provides no evidence of direct-path parity.
4. **Measure compile time.** Use the same shader corpus, device, driver and worker count in both runs. Report cold per-shader compile latency and warm-cache build time separately. The proposed targets are over 2× for cold compilation and over 4× for the parallel warm build. Measure them independently of correctness.
5. **Test rollback with warm and cold caches.** Once implemented, restart with `EDEN_NX2_DIRECT_SM86=0` and verification disabled. Confirm a clean cache and a cache previously populated by the direct compiler both produce reference pipelines. Use separate cache namespaces or reject incompatible entries before reuse. The compile-time fallback is a build configured with `ENABLE_RUST_AMP_SPIRV=OFF`. Retain the Maxwell emitter and its tensor handling while any supported profile uses fallback.
