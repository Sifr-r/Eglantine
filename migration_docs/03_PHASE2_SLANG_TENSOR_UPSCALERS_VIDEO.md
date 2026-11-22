# Phase 2 — Host shaders, tensor instructions, upscaling and video decode

These workstreams are proposals. Each has an independent fallback. A device that supports one acceleration path may still need the reference implementation for another.

## 2A. Compile selected host shaders with Slang

The [host shader build](../src/video_core/host_shaders/CMakeLists.txt) generates C++ headers from GLSL with glslangValidator. Its default target is SPIR-V 1.3. Start with one existing compute shader, such as the BC encoder, and preserve its bindings, entry point and generated header interface.

- Add opt-in Slang compilation to the host shader build. Pin the compiler and checksums for packaged builds, use a host tool in Nix, and keep the GLSL-only build usable offline.
- Target the SPIR-V version and capabilities supported by each shader's runtime path. Validate the binary before embedding it. Compare outputs with the existing kernel, including partial blocks and edge extents.
- After parity, extend coverage to ASTC, block-linear unswizzle, motion vectors and the DLSS network probe. A proposed shared-memory matrix kernel requires a separate dispatch and data-flow design before it can serve guest shaders.

## 2B. Calibrate cooperative matrix lowering

The [Vulkan device probe](../src/video_core/vulkan_common/vulkan_device.cpp) already queries cooperative matrix properties. The [pipeline cache](../src/video_core/renderer_vulkan/vk_pipeline_cache.cpp) also runs a device self-test before enabling the [cooperative HMMA emitter](../src/shader_recompiler/backend/spirv/emit_spirv_tensor.cpp). That emitter assumes a fixed register order.

Extend the existing probe with a profile for the supported tile, element types, subgroup and verified operand/result mapping. Compare basis inputs and mixed operands against the software HMMA path on the actual device. Enable only the shapes and mappings that pass. Cache a successful calibration with its device, driver and lowering version, and invalidate it when any of those change.

Run the [matrix tests](../src/tests/video_core/spirv_software_matrix.cpp) with `tests "[video_core][tensor][ampere]"`. Record the chosen path on each tested GPU. A software fallback result verifies fallback coverage. It does not exercise hardware calibration.

## 2C. Share temporal upscaler inputs

The [Vulkan texture cache](../src/video_core/renderer_vulkan/vk_texture_cache.h) owns separate DLSS and FSR3 implementations. Extract their shared color, depth, motion-vector, exposure or format requirements, jitter, timing and history-reset contract before adding an optional XeSS backend. Keep SDK discovery and backend-specific validation in each implementation.

Preserve existing saved selections and append any new enum values. Test explicit selection and automatic fallback with an absent library, unsupported device, resize and interrupted frame history. Verify the existing DLSS and FSR3 paths with the new backend disabled. Qualify each supported SDK/platform combination before enabling automatic selection there.

## 2D. Prototype Vulkan Video output

The [current decoder](../src/video_core/host1x/nx2_video_decoder.cpp) supports VP8, VP9 and H.264 through FFmpeg. Its render path can receive frames copied back from a host GPU decoder. The proposed optimization is to keep eligible pictures on the GPU through conversion into a guest-visible image.

Probe decode queue support, the specific codec profile, image formats and destination usage. Keep FFmpeg for unsupported streams and destinations, including VP8 where a compatible Vulkan decode path is unavailable. Coordinate decode output and reference-picture lifetimes, queue ownership, layout transitions and guest reads with the [image writer](../src/core/hle/service/nx2_gsv.h). A mapped guest image alone does not establish synchronization.

For each supported codec, compare visible planes, strides, crop and presentation order against FFmpeg. Exercise resize, seek and guest CPU reads. Measure CPU transfers on the same input before claiming a zero-readback path. Disable the experimental decoder and repeat playback to verify rollback.
