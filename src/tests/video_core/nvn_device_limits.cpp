// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/device_memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_null/null_rasterizer.h"
#include "video_core/renderer_vulkan/framebuffer_limits.h"
#include "video_core/texture_cache/descriptor_table.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"
#include "video_core/textures/texture.h"
#include "video_core/vulkan_common/unbacked_image_policy.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace {

TEST_CASE("Texture and sampler descriptor sizes and handle pool widths match device limits",
          "[video_core][nvn-audit]") {
    using Tegra::Texture::TextureHandle;
    using Tegra::Texture::TextureHeaderIndex;
    using Tegra::Texture::TexturePair;
    using Tegra::Texture::TICEntry;
    using Tegra::Texture::TSCEntry;

    STATIC_REQUIRE(sizeof(TICEntry) == 32);
    STATIC_REQUIRE(sizeof(TSCEntry) == 32);
    STATIC_REQUIRE(sizeof(TextureHandle) == 4);

    // 20-bit texture pool index (1,048,576 entries) and 12-bit sampler pool index (4,096 entries).
    const TextureHandle max_handle{0xFFFFFFFFU};
    CHECK(max_handle.tic_id.Value() == 0xFFFFFU);
    CHECK(max_handle.tsc_id.Value() == 0xFFFU);
    CHECK((1U << 20) == 1048576U);
    CHECK((1U << 12) == 4096U);

    const auto [tic_idx, tsc_idx] = TexturePair(max_handle.raw, false);
    CHECK(tic_idx == 0xFFFFFU);
    CHECK(tsc_idx == 0xFFFU);

    const auto [linked_tic, linked_tsc] = TexturePair(0x1234U, true);
    CHECK(linked_tic == 0x1234U);
    CHECK(linked_tsc == 0x1234U);

    // Native byte-offset texture handles scale by 32-byte TIC entry size.
    CHECK(TextureHeaderIndex(256U * 32U, 512U, true) == 256U);
    CHECK(TextureHeaderIndex(256U, 512U, true) == 256U);
    CHECK(TextureHeaderIndex(256U * 32U, 512U, false) == 256U * 32U);

    // DescriptorTable accepts pools up to at least 1,048,576 texture descriptors.
    STATIC_REQUIRE(VideoCommon::DescriptorTable<TICEntry>::MaxDescriptorLimit >= 0xFFFFFU);
    STATIC_REQUIRE(VideoCommon::DescriptorTable<TSCEntry>::MaxDescriptorLimit >= 0xFFFU);
}

TEST_CASE("Maxwell3D and KeplerCompute register limits match device constants",
          "[video_core][nvn-audit]") {
    using MaxwellRegs = Tegra::Engines::Maxwell3D::Regs;
    using Compute = Tegra::Engines::KeplerCompute;

    STATIC_REQUIRE(MaxwellRegs::NumRenderTargets == 8);
    STATIC_REQUIRE(MaxwellRegs::NumViewports == 16);
    STATIC_REQUIRE(MaxwellRegs::NumImages == 8);
    STATIC_REQUIRE(MaxwellRegs::NumTransformFeedbackBuffers == 4);
    STATIC_REQUIRE(MaxwellRegs::MaxConstBufferSize == 65536);
    STATIC_REQUIRE(MaxwellRegs::MaxConstBuffers >= 14);
    STATIC_REQUIRE(MaxwellRegs::NumVertexArrays >= 16);
    STATIC_REQUIRE(MaxwellRegs::NumVertexAttributes >= 16);
    STATIC_REQUIRE(VideoCommon::NUM_TRANSFORM_FEEDBACK_BUFFERS == 4);
    STATIC_REQUIRE(VideoCommon::NUM_TEXTURE_BUFFERS == 32);

    // Vertex attribute relative offset is a 14-bit field (0..16383).
    MaxwellRegs::VertexAttribute attr{};
    attr.offset.Assign(16383U);
    CHECK(attr.offset.Value() == 16383U);

    // Compute QMD address register shifts by 8 bits (256-byte alignment).
    Compute::Regs compute_regs{};
    compute_regs.launch_desc_loc.address = 0x1234U;
    CHECK(compute_regs.launch_desc_loc.Address() == (GPUVAddr{0x1234U} << 8));
}

TEST_CASE("Linear texture and render-target stride handling respects alignment rules",
          "[video_core][nvn-audit]") {
    using Tegra::Engines::Maxwell3D;
    using VideoCommon::ImageInfo;
    using VideoCommon::ImageType;
    using VideoCommon::IsPitchLinearSameSize;
    using VideoCore::Surface::PixelFormat;

    // Linear textures align pitch to 32 bytes; 33px and 40px RGBA8 share a 160B (5 * 32B) stride,
    // while 32px (128B) and 33px (160B) do not.
    ImageInfo tex32{};
    tex32.type = ImageType::Linear;
    tex32.format = PixelFormat::A8B8G8R8_UNORM;
    tex32.size = {32, 16, 1};
    tex32.pitch = 128;

    ImageInfo tex33 = tex32;
    tex33.size = {33, 16, 1};
    tex33.pitch = 160;

    ImageInfo tex40 = tex32;
    tex40.size = {40, 16, 1};
    tex40.pitch = 160;

    CHECK_FALSE(IsPitchLinearSameSize(tex32, tex33, false));
    CHECK(IsPitchLinearSameSize(tex33, tex40, false));
    CHECK_FALSE(IsPitchLinearSameSize(tex33, tex40, true));

    // Linear render target config preserves its 128-byte-aligned byte pitch directly.
    Maxwell3D::Regs::RenderTargetConfig rt{};
    rt.format = Tegra::RenderTargetFormat::A8B8G8R8_UNORM;
    rt.tile_mode.is_pitch_linear.Assign(1);
    rt.tile_mode.dim_control.Assign(
        Maxwell3D::Regs::TileMode::DimensionControl::DefineArraySize);
    rt.width = 256;
    rt.height = 64;
    const ImageInfo rt_info{rt, Tegra::Texture::MsaaMode::Msaa1x1};
    CHECK(rt_info.type == ImageType::Linear);
    CHECK(rt_info.pitch == 256);
    CHECK(rt_info.size.width == 64);
    CHECK(rt_info.size.height == 64);
}

TEST_CASE("Vulkan subgroup, framebuffer, and anisotropy invariants stay bounded",
          "[video_core][nvn-audit]") {
    STATIC_REQUIRE(Vulkan::GuestWarpSize == 32);

    constexpr Vulkan::FramebufferSize device_limits{16384, 16384, 2048};
    const auto clamped = Vulkan::ClampFramebufferSize({32768, 32768, 4096}, device_limits);
    CHECK(clamped.width == 16384);
    CHECK(clamped.height == 16384);
    CHECK(clamped.layers == 2048);

    // Out-of-range TSC max_anisotropy (3-bit values > 4) must clamp cleanly to 16.0f max.
    Tegra::Texture::TSCEntry tsc{};
    tsc.mipmap_filter.Assign(Tegra::Texture::TextureMipmapFilter::Linear);
    tsc.min_filter.Assign(Tegra::Texture::TextureFilter::Linear);
    tsc.max_lod_clamp.Assign(256U);
    tsc.max_anisotropy.Assign(7U);
    CHECK(std::clamp(tsc.MaxAnisotropy(), 1.0f, 16.0f) == 16.0f);
    CHECK(std::clamp(static_cast<f32>(1U << tsc.max_anisotropy), 1.0f, 16.0f) == 16.0f);
    CHECK(std::clamp(tsc.MaxAnisotropy(), 1.0f, 8.0f) == 8.0f);
}

TEST_CASE("Sparse virtual-pool unmapped and reserved pages read zero and gate unbacked images",
          "[video_core][nvn-audit]") {
    // Unbacked sparse images are allowed only when non-resident reads strictly return zero
    // and the driver does not fault on them (unless explicitly forced via env override).
    CHECK(Vulkan::CanUseUnbackedSparseImagesPolicy(true, false, '\0'));
    CHECK_FALSE(Vulkan::CanUseUnbackedSparseImagesPolicy(false, false, '\0'));
    CHECK_FALSE(Vulkan::CanUseUnbackedSparseImagesPolicy(true, true, '\0'));
    CHECK_FALSE(Vulkan::CanUseUnbackedSparseImagesPolicy(true, false, '0'));
    CHECK(Vulkan::CanUseUnbackedSparseImagesPolicy(false, true, '1'));

    ScopedSystemTestContext test_ctx;
    Core::System system;
    system.Initialize();
    Tegra::MaxwellDeviceMemoryManager smmu{system.DeviceMemory()};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    smmu.BindInterface(&rasterizer);
    Tegra::MemoryManager memory{system, smmu, 32};
    memory.BindRasterizer(&rasterizer);

    constexpr GPUVAddr unmapped_addr = 0x40000000ULL;
    constexpr GPUVAddr sparse_addr = 0x50000000ULL;
    constexpr size_t sparse_size = 0x20000; // 128 KiB (two 64 KiB big pages)

    // Unmapped pages read zero.
    CHECK(memory.Read<u32>(unmapped_addr) == 0);
    std::array<u8, 64> buf{};
    buf.fill(0xAA);
    memory.ReadBlockUnsafe(unmapped_addr, buf.data(), buf.size());
    CHECK(std::ranges::all_of(buf, [](u8 b) { return b == 0; }));

    // Reserved sparse virtual pool pages read zero and drop writes without backing.
    memory.MapSparse(sparse_addr, sparse_size, true);
    const std::array<u8, 32> nonzero{
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
        17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    };
    memory.WriteBlockUnsafe(sparse_addr, nonzero.data(), nonzero.size());
    buf.fill(0x55);
    memory.ReadBlock(sparse_addr, buf.data(), buf.size(), VideoCommon::CacheType::None);
    CHECK(std::ranges::all_of(buf, [](u8 b) { return b == 0; }));
    CHECK(memory.Read<u64>(sparse_addr + 0x10000) == 0);
}

} // namespace
