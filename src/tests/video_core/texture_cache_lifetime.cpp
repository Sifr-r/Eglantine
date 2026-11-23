// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/control/channel_state.h"
#include "video_core/control/channel_state_bridge.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
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

VideoCommon::ImageInfo MakeImageInfo() {
    VideoCommon::ImageInfo info{};
    info.type = VideoCommon::ImageType::e2D;
    info.format = VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM;
    info.size = {64, 32, 1};
    info.resources = {1, 1};
    info.num_samples = 1;
    return info;
}

struct Fixture {
    ScopedSystemTestContext context;
    std::unique_ptr<Core::System> system;
    std::unique_ptr<Core::DeviceMemory> physical;
    std::unique_ptr<Tegra::MaxwellDeviceMemoryManager> device;
    std::unique_ptr<Tegra::GPU> gpu;
    std::unique_ptr<Null::RasterizerNull> rasterizer;
    TestRuntime runtime;
    std::unique_ptr<Tegra::MemoryManager> memory;
    std::unique_ptr<Tegra::MemoryManager> other_memory;
    std::unique_ptr<Cache> cache;
    std::unique_ptr<Tegra::Control::ChannelState> channel;
    std::unique_ptr<Tegra::Control::ChannelState> other_channel;

    Fixture() {
        system = std::make_unique<Core::System>();
        physical = std::make_unique<Core::DeviceMemory>();
        device = std::make_unique<Tegra::MaxwellDeviceMemoryManager>(*physical);
        gpu = std::make_unique<Tegra::GPU>(*system, false, false);
        rasterizer = std::make_unique<Null::RasterizerNull>(*gpu);
        memory = std::make_unique<Tegra::MemoryManager>(*system, *device, 32, 1ULL << 31, 16);
        other_memory = std::make_unique<Tegra::MemoryManager>(*system, *device, 32, 1ULL << 31, 16);
        memory->BindRasterizer(rasterizer.get());
        other_memory->BindRasterizer(rasterizer.get());
        cache = std::make_unique<Cache>(runtime, *device);
        channel = std::make_unique<Tegra::Control::ChannelState>(1);
        other_channel = std::make_unique<Tegra::Control::ChannelState>(2);
    }

    void CreateChannels() {
        channel->memory_manager = memory.get();
        other_channel->memory_manager = other_memory.get();
        channel->Init(*system, 0);
        other_channel->Init(*system, 0);
        cache->CreateChannel(*channel);
        cache->CreateChannel(*other_channel);
    }
};

struct WriteBackHarness {
    static constexpr u64 phys = 0x80000;
    static constexpr u64 guest = 0x200000;
    static constexpr u64 extent = 0x20000;
    static constexpr DAddr first_device = 0x100000;
    static constexpr DAddr second_device = 0x110000;

    ScopedSystemTestContext context;
    std::unique_ptr<Core::System> system;
    std::unique_ptr<Kernel::KProcess> process;
    std::unique_ptr<Tegra::MaxwellDeviceMemoryManager> device;
    std::optional<Core::Asid> asid;
    std::unique_ptr<Tegra::GPU> gpu;
    std::unique_ptr<Null::RasterizerNull> rasterizer;
    TestRuntime runtime;

    WriteBackHarness() {
        system = std::make_unique<Core::System>();
        system->Initialize();
        process = std::make_unique<Kernel::KProcess>(system->Kernel());
        auto& cpu = process->GetMemory();
        auto& table = process->GetPageTable().GetImpl();
        table.Resize(32, 12);
        cpu.SetCurrentPageTable(*process);
        system->DeviceMemory().buffer.ClearBackingRegion(phys, extent, 0);
        cpu.MapMemoryRegion(table, Common::ProcessAddress{guest}, extent,
                            Common::PhysicalAddress{Core::DramMemoryMap::Base + phys},
                            Common::MemoryPermission::ReadWrite, false);
        device = std::make_unique<Tegra::MaxwellDeviceMemoryManager>(system->DeviceMemory());
        asid = device->RegisterProcess(&cpu);
        device->Map(first_device, guest, 0x10000, *asid);
        device->Map(second_device, guest + 0x10000, 0x10000, *asid);
        gpu = std::make_unique<Tegra::GPU>(*system, false, false);
        rasterizer = std::make_unique<Null::RasterizerNull>(*gpu);
    }

    ~WriteBackHarness() {
        rasterizer.reset();
        gpu.reset();
        if (asid.has_value() && device) {
            device->UnregisterProcess(*asid);
        }
        device.reset();
        if (process && system) {
            system->Kernel().UnregisterKernelObject(process.get());
        }
        process.reset();
        system.reset();
    }

    [[nodiscard]] bool DeviceContains(DAddr address, u8 value) const {
        std::vector<u8> bytes(64 * 32 * 4);
        device->ReadBlockUnsafe(address, bytes.data(), bytes.size());
        return std::ranges::find(bytes, value) != bytes.end();
    }
};

} // namespace

namespace VideoCommon {

struct TextureCacheTestAccess {
    template <typename F>
    static void ForEachSparseSegment(Cache& cache, Tegra::MemoryManager& memory, ImageBase& image,
                                     F&& visit) {
        cache.gpu_memory = &memory;
        cache.ForEachSparseSegment(image, std::forward<F>(visit));
    }

    static ImageId Register(Cache& cache, const ImageInfo& info, GPUVAddr gpu_addr, DAddr cpu_addr,
                            ImageFlagBits extra_flags = ImageFlagBits{}) {
        const ImageId image_id = cache.slot_images.insert(cache.runtime, info, gpu_addr, cpu_addr);
        cache.slot_images[image_id].flags |= extra_flags;
        cache.RegisterImage(image_id);
        return image_id;
    }

    static void Unregister(Cache& cache, ImageId image_id) {
        cache.UnregisterImage(image_id);
    }

    static bool WriteBack(Cache& cache, ImageId image_id, const std::vector<u8>& data) {
        const ImageBase& image = cache.slot_images[image_id];
        return cache.WriteBackImage(image, FullDownloadCopies(image.info), data);
    }

    static size_t PagedImages(Cache& cache, size_t address_space_index) {
        size_t count{};
        for (const auto& [page, images] : cache.gpu_page_table_storage[address_space_index * 2]) {
            count += images.size();
        }
        return count;
    }
};

} // namespace VideoCommon

TEST_CASE("Sparse texture iteration skips stale translations and visits later segments",
          "[video_core][lifetime][texture_cache][sparse_segments]") {
    auto fixture = std::make_unique<Fixture>();
    constexpr GPUVAddr base = 0x200000;
    constexpr size_t page_size = 0x1000;
    constexpr DAddr invalid = DAddr{1} << Tegra::MaxwellDeviceMemoryManager::AS_BITS;
    constexpr DAddr valid = 0x20000;
    fixture->memory->Map(base, invalid, page_size, Tegra::PTEKind::PITCH, false);
    fixture->memory->Map(base + page_size, valid, page_size, Tegra::PTEKind::PITCH, false);

    VideoCommon::ImageBase image{MakeImageInfo(), base, valid};
    REQUIRE(fixture->memory->GetSubmappedRange(base, image.guest_size_bytes).size() == 2);
    REQUIRE_FALSE(fixture->memory->GpuToCpuAddress(base));

    std::vector<GPUVAddr> visited;
    VideoCommon::TextureCacheTestAccess::ForEachSparseSegment(
        *fixture->cache, *fixture->memory, image,
        [&](GPUVAddr address, DAddr device, size_t size) {
            visited.push_back(address);
            CHECK(device == valid);
            CHECK(size == page_size);
        });
    CHECK(visited == std::vector<GPUVAddr>{base + page_size});
}

TEST_CASE("UnregisterImage clears the page table of the address space the image was registered in",
          "[video_core][lifetime][texture_cache][address_space]") {
    auto fixture = std::make_unique<Fixture>();
    fixture->CreateChannels();
    constexpr GPUVAddr gpu_addr = 0x200000;
    constexpr DAddr cpu_addr = 0x20000;

    fixture->cache->BindToChannel(2);
    const auto image_id = VideoCommon::TextureCacheTestAccess::Register(
        *fixture->cache, MakeImageInfo(), gpu_addr, cpu_addr);
    REQUIRE(VideoCommon::TextureCacheTestAccess::PagedImages(*fixture->cache, 1) == 1);

    fixture->cache->BindToChannel(1);
    VideoCommon::TextureCacheTestAccess::Unregister(*fixture->cache, image_id);
    CHECK(VideoCommon::TextureCacheTestAccess::PagedImages(*fixture->cache, 0) == 0);
    CHECK(VideoCommon::TextureCacheTestAccess::PagedImages(*fixture->cache, 1) == 0);
}

TEST_CASE("UnregisterImage clears the page table for channels created from ChannelStateBridge",
          "[video_core][lifetime][texture_cache][address_space][channel_bridge]") {
    auto fixture = std::make_unique<Fixture>();
    auto maxwell1 = std::make_unique<Tegra::Engines::Maxwell3D>(*fixture->memory);
    auto compute1 = std::make_unique<Tegra::Engines::KeplerCompute>(*fixture->memory);
    auto maxwell2 = std::make_unique<Tegra::Engines::Maxwell3D>(*fixture->other_memory);
    auto compute2 = std::make_unique<Tegra::Engines::KeplerCompute>(*fixture->other_memory);

    fixture->cache->CreateChannelFromBridge(
        Tegra::Control::ChannelStateBridge{*maxwell1, *compute1, *fixture->memory, 0, 1});
    fixture->cache->CreateChannelFromBridge(
        Tegra::Control::ChannelStateBridge{*maxwell2, *compute2, *fixture->other_memory, 0, 2});

    constexpr GPUVAddr gpu_addr = 0x200000;
    constexpr DAddr cpu_addr = 0x20000;
    fixture->cache->BindToChannel(2);
    const auto image_id = VideoCommon::TextureCacheTestAccess::Register(
        *fixture->cache, MakeImageInfo(), gpu_addr, cpu_addr);
    REQUIRE(VideoCommon::TextureCacheTestAccess::PagedImages(*fixture->cache, 1) == 1);

    fixture->cache->BindToChannel(1);
    VideoCommon::TextureCacheTestAccess::Unregister(*fixture->cache, image_id);
    CHECK(VideoCommon::TextureCacheTestAccess::PagedImages(*fixture->cache, 0) == 0);
    CHECK(VideoCommon::TextureCacheTestAccess::PagedImages(*fixture->cache, 1) == 0);
    fixture->cache->EraseChannel(1);
    fixture->cache->EraseChannel(2);
}

TEST_CASE("WriteBackImage writes through the memory manager of the image's own address space",
          "[video_core][lifetime][texture_cache][address_space]") {
    auto harness = std::make_unique<WriteBackHarness>();
    const auto first = std::make_shared<Tegra::MemoryManager>(*harness->system, *harness->device);
    auto second = std::make_shared<Tegra::MemoryManager>(*harness->system, *harness->device);
    first->BindRasterizer(harness->rasterizer.get());
    second->BindRasterizer(harness->rasterizer.get());
    constexpr GPUVAddr gpu_addr = 0x400000;
    first->Map(gpu_addr, WriteBackHarness::first_device, 0x10000, Tegra::PTEKind::PITCH, false);
    second->Map(gpu_addr, WriteBackHarness::second_device, 0x10000, Tegra::PTEKind::PITCH, false);
    auto first_channel = std::make_unique<Tegra::Control::ChannelState>(1);
    auto second_channel = std::make_unique<Tegra::Control::ChannelState>(2);
    first_channel->memory_manager = first.get();
    second_channel->memory_manager = second.get();
    first_channel->Init(*harness->system, 0);
    second_channel->Init(*harness->system, 0);
    auto cache = std::make_unique<Cache>(harness->runtime, *harness->device);
    cache->CreateChannel(*first_channel);
    cache->CreateChannel(*second_channel);

    cache->BindToChannel(2);
    const auto image_id = VideoCommon::TextureCacheTestAccess::Register(
        *cache, MakeImageInfo(), gpu_addr, WriteBackHarness::second_device);
    cache->BindToChannel(1);

    const auto data_of = [](u8 value) { return std::vector<u8>(64 * 32 * 4, value); };
    CHECK(VideoCommon::TextureCacheTestAccess::WriteBack(*cache, image_id, data_of(0xAB)));
    CHECK(harness->DeviceContains(WriteBackHarness::second_device, 0xAB));
    CHECK_FALSE(harness->DeviceContains(WriteBackHarness::first_device, 0xAB));

    cache->EraseChannel(2);
    second_channel.reset();
    second.reset();
    CHECK_FALSE(VideoCommon::TextureCacheTestAccess::WriteBack(*cache, image_id, data_of(0xEF)));
    CHECK_FALSE(harness->DeviceContains(WriteBackHarness::second_device, 0xEF));
    CHECK_FALSE(harness->DeviceContains(WriteBackHarness::first_device, 0xEF));

    first->Unmap(gpu_addr, 0x10000);
    first->Map(gpu_addr, WriteBackHarness::second_device, 0x10000, Tegra::PTEKind::PITCH, false);
    CHECK(VideoCommon::TextureCacheTestAccess::WriteBack(*cache, image_id, data_of(0xCD)));
    CHECK(harness->DeviceContains(WriteBackHarness::second_device, 0xCD));
    CHECK_FALSE(harness->DeviceContains(WriteBackHarness::first_device, 0xCD));
}

TEST_CASE("WriteBackImage via ChannelStateBridge tracks weak_ptr lifetime and drops closed sparse images",
          "[video_core][lifetime][texture_cache][address_space][channel_bridge]") {
    auto harness = std::make_unique<WriteBackHarness>();
    const auto first = std::make_shared<Tegra::MemoryManager>(*harness->system, *harness->device);
    auto second = std::make_shared<Tegra::MemoryManager>(*harness->system, *harness->device);
    first->BindRasterizer(harness->rasterizer.get());
    second->BindRasterizer(harness->rasterizer.get());
    constexpr GPUVAddr gpu_addr = 0x400000;
    first->Map(gpu_addr, WriteBackHarness::first_device, 0x10000, Tegra::PTEKind::PITCH, false);
    second->Map(gpu_addr, WriteBackHarness::second_device, 0x10000, Tegra::PTEKind::PITCH, false);

    auto maxwell1 = std::make_unique<Tegra::Engines::Maxwell3D>(*first);
    auto compute1 = std::make_unique<Tegra::Engines::KeplerCompute>(*first);
    auto maxwell2 = std::make_unique<Tegra::Engines::Maxwell3D>(*second);
    auto compute2 = std::make_unique<Tegra::Engines::KeplerCompute>(*second);
    auto cache = std::make_unique<Cache>(harness->runtime, *harness->device);
    cache->CreateChannelFromBridge(
        Tegra::Control::ChannelStateBridge{*maxwell1, *compute1, *first, 0, 1});
    cache->CreateChannelFromBridge(
        Tegra::Control::ChannelStateBridge{*maxwell2, *compute2, *second, 0, 2});

    cache->BindToChannel(2);
    const auto regular_id = VideoCommon::TextureCacheTestAccess::Register(
        *cache, MakeImageInfo(), gpu_addr, WriteBackHarness::second_device);
    const auto sparse_id = VideoCommon::TextureCacheTestAccess::Register(
        *cache, MakeImageInfo(), gpu_addr, WriteBackHarness::second_device,
        VideoCommon::ImageFlagBits::Sparse);
    cache->BindToChannel(1);

    const auto data_of = [](u8 value) { return std::vector<u8>(64 * 32 * 4, value); };
    CHECK(VideoCommon::TextureCacheTestAccess::WriteBack(*cache, regular_id, data_of(0x51)));
    CHECK(harness->DeviceContains(WriteBackHarness::second_device, 0x51));
    CHECK_FALSE(harness->DeviceContains(WriteBackHarness::first_device, 0x51));

    cache->EraseChannel(2);
    compute2.reset();
    maxwell2.reset();
    second.reset();
    first->Unmap(gpu_addr, 0x10000);
    first->Map(gpu_addr, WriteBackHarness::second_device, 0x10000, Tegra::PTEKind::PITCH, false);
    CHECK(VideoCommon::TextureCacheTestAccess::WriteBack(*cache, regular_id, data_of(0x62)));
    CHECK(harness->DeviceContains(WriteBackHarness::second_device, 0x62));
    CHECK_FALSE(VideoCommon::TextureCacheTestAccess::WriteBack(*cache, sparse_id, data_of(0x73)));
    CHECK_FALSE(harness->DeviceContains(WriteBackHarness::second_device, 0x73));
}
