# `shader_recompiler` — Maxwell/Ampere Lowering & Direct SPIR-V

Entry points: `src/shader_recompiler/frontend/ampere/lower_to_maxwell.h`, `src/video_core/shader_environment.{h,cpp}`, `src/video_core/shader_cache.{h,cpp}`.

## Lazy Lowering & Direct SM86 Compute

- **Lazy SM86 lowering:** `GenericEnvironment` / `ShaderCache` capture native SM86 instructions and `NativeProgramKey` before calling thread-safe `Shader::Ampere::LowerToMaxwell` (tracked by `ResetLowerToMaxwellInvocationCount` / `LowerToMaxwellInvocationCount`).
- **Direct SM86 compute bypass:** `Vulkan::PipelineCache` (`vk_pipeline_cache.{h,cpp}`) gates direct compilation via `DirectSm86SupportsStage` (`Compute` only) and `EDEN_NX2_DIRECT_SM86=1`, invoking `TryCompileDirectSm86` before Maxwell CFG/lowering (`0` `LowerToMaxwell` calls) on cold and warm SM86 compute shaders (`ComputePipeline::NativeKey()` / `BuildsUnoptimized` in `vk_compute_pipeline.{h,cpp}`, `pipeline_optimization.h`). Graphics stages remain lazily lowered via Maxwell.
- **Warm precache alignment:** `ComputeNativeDiscoveryHash` and `PackNativeSm86Blob` / `TryUnpackNativeSm86Blob` align warm compute entries (`WarmCacheVersion = 36`, `WarmBuilderVersion = 4` in `src/video_core/shader_warm/warm_records.{h,cpp}`, `shader_warm_builder.cpp`) with runtime discovery keys.
- **Rejection & eviction:** `ShaderCache` and `Vulkan::PipelineCache::OnDebugRejectionsRefreshed` evict rejected/invalid pipelines; `EvictFailedLazyShader` / `EvictFailedLazyShaders` / `TryGetResolved` (null-guarding `gpu_memory`) evict failed lazy `ShaderInfo` entries; `ShaderCache::ComputeNativeKey()` reports bound compute keys.

## Cross-Language & IR Invariants

- **Direct SPIR-V FFI error contract:** `rust/crates/ampere_spirv/src/ffi.rs` populates `out.error` / `out.error_len` on hard errors (`rc == -1` or empty SPIR-V) while keeping `out.error == nullptr` / `out.error_len == 0` on graceful `FallbackToMaxwell`.
- **Geometry exit envelope:** `ProgramCaptureState` recognizes the geometry preamble and captures through the exit handler, including when the body uses a computed branch. Experimental geometry stage is opt-in via `EDEN_NX2_EXPERIMENTAL_GEOMETRY=1`.
- **IR edge invariants:**
  - `ConstantPropagationPass` (`ir_opt/constant_propagation_pass.cpp`) handles zero-width and 32-bit `BitFieldUExtract` without shifting by 32.
  - `TexturePass::TryGetConstBuffer()` (`ir_opt/texture_pass.cpp`) tracks `CompositeExtractU32x2` word byte offsets.
  - `IR::Inst::SetArg()` / `AddPhiOperand()` (`frontend/ir/microinstruction.cpp`) resolve immediate `Phi` operands before DCE.
  - SPIR-V `PatchPhiNodes()` (`backend/spirv/emit_spirv.cpp`) rejects missing definitions.

## Verification

- `./build/bin/tests "[ampere]"` and `[ampere-lazy-lowering]`. Use `EDEN_NX2_DIRECT_SM86=1` to cover the Rust direct compute path.
- Related test files: `src/tests/video_core/{ampere_lazy_lowering,ampere_direct_spirv,ampere_geometry,ampere_program,shader_precache,shader_precache_identity,shader_ir_edges}.cpp`.
- Rust suite: `cargo test --manifest-path rust/Cargo.toml --locked --offline`.
