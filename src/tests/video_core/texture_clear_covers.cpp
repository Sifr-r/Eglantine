// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/device_memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/control/channel_state.h"
#include "video_core/control/channel_state_bridge.h"
#include "video_core/gpu.h"
#include "video_core/renderer_null/null_rasterizer.h"
#include "video_core/texture_cache/texture_cache.h"

namespace {

struct TestRuntime {};

struct TestImage : VideoCommon::ImageBase {
    explicit TestImage(const VideoCommon::NullImageParams& params) : ImageBase(params) {}
    TestImage(TestRuntime&, const VideoCommon::ImageInfo& info, GPUVAddr gpu_addr, VAddr cpu_addr)
        : ImageBase(info, gpu_addr, cpu_addr) {}
};

struct TestImageView : VideoCommon::ImageViewBase {
    TestImageView(TestRuntime&, const VideoCommon::NullImageViewParams& params)
        : ImageViewBase(params) {}
    template <class Images>
    TestImageView(TestRuntime&, const VideoCommon::ImageViewInfo& info, VideoCommon::ImageId id,
                  VideoCommon::ImageBase& image, Images&)
        : ImageViewBase(info, image.info, id, image.gpu_addr) {}
};

struct TestSampler {
    TestSampler(TestRuntime&, const Tegra::Texture::TSCEntry&) {}
};

struct Policy {
    using Runtime = TestRuntime;
    using Image = TestImage;
    using ImageAlloc = VideoCommon::ImageAllocBase;
    using ImageView = TestImageView;
    using Sampler = TestSampler;
    struct Framebuffer {};
    struct AsyncBuffer {};
    struct BufferType {};
    static constexpr bool ENABLE_VALIDATION = false;
    static constexpr bool FRAMEBUFFER_BLITS = false;
    static constexpr bool HAS_EMULATED_COPIES = false;
    static constexpr bool HAS_DEVICE_MEMORY_INFO = false;
    static constexpr bool IMPLEMENTS_ASYNC_DOWNLOADS = false;
    static constexpr bool HAS_MSAA_DOWNLOADS = false;
};

using Cache = VideoCommon::TextureCache<Policy>;

VideoCommon::ImageInfo MakeImageInfo(u32 width = 64, u32 height = 32) {
    VideoCommon::ImageInfo info{};
    info.type = VideoCommon::ImageType::e2D;
    info.format = VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM;
    info.size = {width, height, 1};
    info.resources = {1, 1};
    info.num_samples = 1;
    return info;
}

struct Fixture {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device{physical};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    TestRuntime runtime;
    Tegra::MemoryManager memory{system, device, 32, 1ULL << 31, 16};
    Cache cache{runtime, device};
    Tegra::Control::ChannelState channel{1};

    Fixture() {
        memory.BindRasterizer(&rasterizer);
        channel.memory_manager = &memory;
        channel.Init(system, 0);
        cache.CreateChannel(channel);
        cache.BindToChannel(channel.bind_id);
    }
};

} // namespace

namespace VideoCommon {

struct TextureCacheTestAccess {
    static ImageId Register(Cache& cache, const ImageInfo& info, GPUVAddr gpu_addr, DAddr cpu_addr) {
        const ImageId image_id = cache.slot_images.insert(cache.runtime, info, gpu_addr, cpu_addr);
        cache.RegisterImage(image_id);
        return image_id;
    }

    static bool ClearCovers(const Cache& cache, u32 width, u32 height) {
        return cache.ClearCovers(width, height);
    }

    static bool IsFullClear(Cache& cache, ImageId image_id, Extent2D render_size = {0, 0}) {
        ImageBase& image = cache.slot_images[image_id];
        ImageViewInfo info{ImageViewType::e2D, image.info.format};
        info.render_size = render_size;
        return cache.IsFullClear(cache.slot_image_views.insert(cache.runtime, info, image_id, image,
                                                               cache.slot_images));
    }
};

} // namespace VideoCommon

TEST_CASE("ClearCovers and IsFullClear require surface_clip to reach the whole image",
          "[video_core][texture_cache][clear_covers]") {
    Fixture fixture;
    const auto image_id = VideoCommon::TextureCacheTestAccess::Register(
        fixture.cache, MakeImageInfo(64, 32), 0x200000, 0x20000);
    auto& regs = fixture.channel.payload->maxwell_3d.regs;
    regs.clear_control.use_scissor.Assign(0);
    regs.surface_clip.width.Assign(64);
    regs.surface_clip.height.Assign(32);

    CHECK(VideoCommon::TextureCacheTestAccess::ClearCovers(fixture.cache, 64, 32));
    CHECK(VideoCommon::TextureCacheTestAccess::IsFullClear(fixture.cache, image_id));

    regs.surface_clip.width.Assign(32);
    regs.surface_clip.height.Assign(16);
    CHECK_FALSE(VideoCommon::TextureCacheTestAccess::ClearCovers(fixture.cache, 64, 32));
    CHECK_FALSE(VideoCommon::TextureCacheTestAccess::IsFullClear(fixture.cache, image_id));

    regs.surface_clip.width.Assign(32);
    regs.surface_clip.height.Assign(32);
    CHECK_FALSE(VideoCommon::TextureCacheTestAccess::ClearCovers(fixture.cache, 64, 32));
    CHECK_FALSE(VideoCommon::TextureCacheTestAccess::IsFullClear(fixture.cache, image_id));

    regs.surface_clip.width.Assign(64);
    regs.surface_clip.height.Assign(16);
    CHECK_FALSE(VideoCommon::TextureCacheTestAccess::ClearCovers(fixture.cache, 64, 32));
    CHECK_FALSE(VideoCommon::TextureCacheTestAccess::IsFullClear(fixture.cache, image_id));
}

TEST_CASE("ClearCovers flips Y scissor around surface_clip for lower-left window origin",
          "[video_core][texture_cache][clear_covers]") {
    using WindowOrigin = Tegra::Engines::Maxwell3D::Regs::WindowOrigin;
    Fixture fixture;
    const auto image_id = VideoCommon::TextureCacheTestAccess::Register(
        fixture.cache, MakeImageInfo(64, 32), 0x200000, 0x20000);
    auto& regs = fixture.channel.payload->maxwell_3d.regs;
    regs.clear_control.use_scissor.Assign(1);
    regs.surface_clip.width.Assign(64);
    regs.surface_clip.height.Assign(64);
    regs.scissor_test[0].enable = 1;
    regs.scissor_test[0].min_x.Assign(0);
    regs.scissor_test[0].min_y.Assign(0);
    regs.scissor_test[0].max_x.Assign(64);
    regs.scissor_test[0].max_y.Assign(32);

    regs.window_origin.mode.Assign(WindowOrigin::Mode::UpperLeft);
    CHECK(VideoCommon::TextureCacheTestAccess::ClearCovers(fixture.cache, 64, 32));
    CHECK(VideoCommon::TextureCacheTestAccess::IsFullClear(fixture.cache, image_id));

    // With LowerLeft origin, scissor [0, 32) inside a 64-high clip flips to [32, 64) and misses
    // [0, 32) of the target.
    regs.window_origin.mode.Assign(WindowOrigin::Mode::LowerLeft);
    CHECK_FALSE(VideoCommon::TextureCacheTestAccess::ClearCovers(fixture.cache, 64, 32));
    CHECK_FALSE(VideoCommon::TextureCacheTestAccess::IsFullClear(fixture.cache, image_id));

    // When scissor.enable is 0, no Y-flip is applied.
    regs.scissor_test[0].enable = 0;
    CHECK(VideoCommon::TextureCacheTestAccess::ClearCovers(fixture.cache, 64, 32));
    CHECK(VideoCommon::TextureCacheTestAccess::IsFullClear(fixture.cache, image_id));

    // Covering the full clip height [0, 64) covers [0, 32) after the Y-flip too.
    regs.scissor_test[0].enable = 1;
    regs.scissor_test[0].max_y.Assign(64);
    CHECK(VideoCommon::TextureCacheTestAccess::ClearCovers(fixture.cache, 64, 32));
    CHECK(VideoCommon::TextureCacheTestAccess::IsFullClear(fixture.cache, image_id));

    // A non-zero min_y still fails.
    regs.scissor_test[0].min_y.Assign(40);
    CHECK_FALSE(VideoCommon::TextureCacheTestAccess::ClearCovers(fixture.cache, 64, 32));
    CHECK_FALSE(VideoCommon::TextureCacheTestAccess::IsFullClear(fixture.cache, image_id));
}

TEST_CASE("IsFullClear preserves RenderSubExtent guard and works via ChannelStateBridge",
          "[video_core][texture_cache][clear_covers]") {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device{physical};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    TestRuntime runtime;
    Tegra::MemoryManager memory{system, device, 32, 1ULL << 31, 16};
    memory.BindRasterizer(&rasterizer);
    Cache cache{runtime, device};

    Tegra::Control::ChannelState channel{2};
    channel.memory_manager = &memory;
    channel.Init(system, 0);
    const Tegra::Control::ChannelStateBridge bridge{
        channel.payload->maxwell_3d, channel.payload->kepler_compute, memory, 0, channel.bind_id};
    cache.CreateChannelFromBridge(bridge);
    cache.BindToChannel(channel.bind_id);

    const auto image_id = VideoCommon::TextureCacheTestAccess::Register(
        cache, MakeImageInfo(64, 32), 0x200000, 0x20000);
    auto& regs = channel.payload->maxwell_3d.regs;
    regs.clear_control.use_scissor.Assign(0);
    regs.surface_clip.width.Assign(64);
    regs.surface_clip.height.Assign(32);

    CHECK(VideoCommon::TextureCacheTestAccess::IsFullClear(cache, image_id));
    CHECK_FALSE(VideoCommon::TextureCacheTestAccess::IsFullClear(cache, image_id, {32, 16}));
}
