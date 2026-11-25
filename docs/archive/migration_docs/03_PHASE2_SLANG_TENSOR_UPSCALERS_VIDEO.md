# Phase 2 — Host shaders, tensor instructions, upscaling and video decode

This document records the original Phase 2 proposals. See the [architecture ledger](../ARCHITECTURE.md#first-party-language-and-domain-ownership) for current ownership and runtime switches. Establish a new [baseline](00_PREREQUISITES_AND_BASELINE.md) before comparing an acceleration path with its fallback.

## 2A. Compile selected host shaders with Slang

**Status:** The optional Slang build and its unreferenced modules were removed. The [host shader target](../src/video_core/host_shaders/CMakeLists.txt) uses GLSL. The steps below record the original Slang proposal.

The [host shader build](../src/video_core/host_shaders/CMakeLists.txt) generates C++ headers from GLSL with glslangValidator. Its default target is SPIR-V 1.3. Start with one existing compute shader, such as the BC encoder, and preserve its bindings, entry point and generated header interface.

- Add opt-in Slang compilation to the host shader build. Pin the compiler and checksums for packaged builds, use a host tool in Nix, and keep the GLSL-only build usable offline.
- Target the SPIR-V version and capabilities supported by each shader's runtime path. Validate the binary before embedding it. Compare outputs with the existing kernel, including partial blocks and edge extents.
- After parity, extend coverage to ASTC, block-linear unswizzle, motion vectors and the DLSS network probe. A proposed shared-memory matrix kernel requires a separate dispatch and data-flow design before it can serve guest shaders.

## 2B. Calibrate cooperative matrix lowering

**Status:** The [Vulkan device probe](../src/video_core/vulkan_common/vulkan_device.cpp) queries cooperative matrix properties. A [device self-test](../src/video_core/renderer_vulkan/vk_cooperative_hmma_test.cpp) now provides calibration data for the [cooperative HMMA emitter](../src/shader_recompiler/backend/spirv/emit_spirv_tensor.cpp).

The original design called for a profile of the supported tile, element types, subgroup and verified operand/result mapping. Compare basis inputs and mixed operands against the software HMMA path on the actual device. Check that cached calibration matches the device, driver and lowering version.

Run the [matrix tests](../src/tests/video_core/spirv_software_matrix.cpp) with `tests "[video_core][tensor][ampere]"`. Record the chosen path on each tested GPU. A software fallback result verifies fallback coverage. It does not exercise hardware calibration.

## 2C. Share temporal upscaler inputs

**Status:** DLSS, FSR3 and optional XeSS implementations share [ITemporalUpscaler](../src/video_core/renderer_vulkan/vk_temporal_upscaler.h). The [Vulkan texture cache](../src/video_core/renderer_vulkan/vk_texture_cache.h) owns backend instances. Changes to shared color, depth, motion vectors, exposure, formats, jitter, timing or history reset belong at that boundary. SDK discovery and device validation stay in each backend.

Preserve existing saved selections and append any new enum values. Test explicit selection and automatic fallback with an absent library, unsupported device, resize and interrupted frame history. Verify the existing DLSS and FSR3 paths with the new backend disabled. Qualify each supported SDK/platform combination before enabling automatic selection there.

## 2D. Prototype Vulkan Video output

**Status:** The [decoder](../src/video_core/host1x/nx2_video_decoder.cpp) supports VP8, VP9 and H.264 through FFmpeg. `EDEN_NX2_VK_VIDEO=1` stages capability selection for eligible H.264 sessions. Decode and drain still delegate to FFmpeg. Keeping pictures on the GPU through conversion into a guest-visible image is a remaining proposal.

Probe decode queue support, the specific codec profile, image formats and destination usage. Keep FFmpeg for unsupported streams and destinations, including VP8 where a compatible Vulkan decode path is unavailable. Coordinate decode output and reference-picture lifetimes, queue ownership, layout transitions and guest reads with the [image writer](../src/core/hle/service/nx2_gsv.h). A mapped guest image alone does not establish synchronization.

For each supported codec, compare visible planes, strides, crop and presentation order against FFmpeg. Exercise resize, seek and guest CPU reads. Measure CPU transfers on the same input before claiming a zero-readback path. Disable the experimental decoder and repeat playback to verify rollback.
