// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/device_memory.h"
#include "core/device_memory_manager.h"
#include "core/file_sys/program_metadata.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/heap_mapper.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
#include "core/hle/service/nvdrv/nvdata.h"
#include "core/hle/service/nx2_gsv.h"
#include "core/hle/service/os/process.h"
#include "core/loader/loader.h"
#include "tests/core/system_test_context.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/host1x/host1x.h"

namespace {

class TestNvdrvProcessLoader final : public Loader::AppLoader {
public:
    TestNvdrvProcessLoader() : AppLoader(nullptr) {}

    Loader::FileType GetFileType() const override {
        return Loader::FileType::Unknown;
    }

    LoadResult Load(Kernel::KProcess& process, Core::System& system) override {
        const auto metadata = FileSys::ProgramMetadata::GetDefault();
        REQUIRE(process.LoadFromMetadata(system.Kernel(), metadata, Kernel::PageSize, {}, 0)
                    .IsSuccess());
        REQUIRE(process.IsInitialized());
        return {Loader::ResultStatus::Success, LoadParameters{0x2c, 0x100000}};
    }
};

struct NvdrvTestHarness {
    ScopedSystemTestContext context;
    Core::System system;
    std::optional<Service::Process> process;
    std::unique_ptr<Tegra::Host1x::Host1x> host1x;
    std::optional<Service::Nvidia::NvCore::Container> container;

    NvdrvTestHarness() {
        system.Initialize();
        system.Kernel().Initialize();

        process.emplace(system);
        Loader::ResultStatus status = Loader::ResultStatus::Success;
        TestNvdrvProcessLoader loader;
        REQUIRE(process->Initialize(loader, status));
        REQUIRE(process->IsInitialized());

        host1x = std::make_unique<Tegra::Host1x::Host1x>(system);
        container.emplace(*host1x);
    }

    ~NvdrvTestHarness() {
        container.reset();
        host1x.reset();
        process.reset();
        system.ShutdownMainProcess();
    }

    NvdrvTestHarness(const NvdrvTestHarness&) = delete;
    NvdrvTestHarness& operator=(const NvdrvTestHarness&) = delete;
};

} // Anonymous namespace

TEST_CASE("NX2 image pool counts stay inside the pool's byte budget", "[nx2_gsv]") {
    // A pool of 4 MiB images: the count that fills the budget is the budget over the image size.
    constexpr u64 image_size = 0x400000;
    constexpr u32 limit = static_cast<u32>(Service::Nx2MaxImagePoolBytes / image_size);
    REQUIRE(limit < Service::Nx2MaxImageCount);
    CHECK(Service::Nx2ImagePoolCountLimit(image_size) == limit);

    // A count that fits is accepted, one past it is not.
    CHECK(Service::Nx2ImagePoolCountValid(limit, image_size));
    CHECK_FALSE(Service::Nx2ImagePoolCountValid(limit + 1, image_size));

    // A guest that asks for 0xffffffff images is rejected instead of allocating.
    CHECK_FALSE(Service::Nx2ImagePoolCountValid(std::numeric_limits<u32>::max(), image_size));
    CHECK_FALSE(Service::Nx2ImagePoolCountValid(std::numeric_limits<u32>::max(), 0x10000000));
}

TEST_CASE("NX2 image pool counts are capped whatever the image size", "[nx2_gsv]") {
    // Tiny images: the byte budget would allow more images than the handle space does.
    CHECK(Service::Nx2ImagePoolCountLimit(1) == Service::Nx2MaxImageCount);
    CHECK(Service::Nx2ImagePoolCountLimit(0) == Service::Nx2MaxImageCount);
    CHECK(Service::Nx2ImagePoolCountValid(Service::Nx2MaxImageCount, 1));
    CHECK_FALSE(Service::Nx2ImagePoolCountValid(Service::Nx2MaxImageCount + 1, 1));

    // Huge images: a single one already fills the budget.
    CHECK(Service::Nx2ImagePoolCountLimit(Service::Nx2MaxImagePoolBytes) == 1);
    CHECK(Service::Nx2ImagePoolCountValid(1, Service::Nx2MaxImagePoolBytes));
    CHECK_FALSE(Service::Nx2ImagePoolCountValid(2, Service::Nx2MaxImagePoolBytes));

    // A count of zero still allocates one image, so oversized images are rejected.
    CHECK(Service::Nx2ImagePoolCountValid(0, Service::Nx2MaxImagePoolBytes));
    CHECK_FALSE(Service::Nx2ImagePoolCountValid(0, Service::Nx2MaxImagePoolBytes + 1));
    CHECK_FALSE(Service::Nx2ImagePoolBufferSize(Service::Nx2MaxImagePoolBytes + 1, 0).has_value());
}

TEST_CASE("NX2 GPU memory ranges must not run backwards", "[nx2_gsv]") {
    constexpr u64 space_start = 0;
    constexpr u64 space_end = 0x8000000000ULL; // a 39-bit address space

    CHECK(Service::Nx2GpuMemoryRangeValid(0x8000000, 0x8000000 + 0xfff, space_start, space_end));
    CHECK(Service::Nx2GpuMemoryRangeValid(0x8000000, 0x8000000, space_start, space_end));

    // last < start: the size (last - start + 1) would underflow.
    CHECK_FALSE(Service::Nx2GpuMemoryRangeValid(0x8001000, 0x8000000, space_start, space_end));
    CHECK_FALSE(Service::Nx2GpuMemoryRangeValid(1, 0, space_start, space_end));
    CHECK_FALSE(Service::Nx2GpuMemoryRangeValid(std::numeric_limits<u64>::max(), 0, space_start,
                                                 space_end));
}

TEST_CASE("NX2 GPU memory ranges must lie inside the address space", "[nx2_gsv]") {
    constexpr u64 space_start = 0x100000;
    constexpr u64 space_end = 0x100000 + 0x10000000;

    CHECK(Service::Nx2GpuMemoryRangeValid(space_start, space_end - 1, space_start, space_end));
    // The last byte of the space is inside it, the first one past it is not.
    CHECK_FALSE(Service::Nx2GpuMemoryRangeValid(space_end, space_end + 0xfff, space_start,
                                                space_end));
    CHECK_FALSE(Service::Nx2GpuMemoryRangeValid(space_start - 0x1000, space_start, space_start,
                                                space_end));
    // A range that starts inside and ends outside is not accepted either.
    CHECK_FALSE(Service::Nx2GpuMemoryRangeValid(space_end - 0x1000, space_end + 0x1000,
                                                space_start, space_end));
}

TEST_CASE("NX2 GPU-only regions stay within the GPU aperture", "[nx2_gsv]") {
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0x8000000, 0x8000fff) == ResultSuccess);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0x8100000000, 0x8100000000 + (300ULL << 20) - 1) ==
          ResultSuccess);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0xbffffff000, 0xbfffffffff) == ResultSuccess);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0x8001000, 0x8000fff) ==
          Kernel::ResultInvalidAddress);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0x7fff000, 0x7ffffff) ==
          Kernel::ResultInvalidAddress);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0xbffffff000, 0xc000000fff) ==
          Kernel::ResultInvalidAddress);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0x1000, 0xfffffffffffff00f) ==
          Kernel::ResultInvalidAddress);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0x1000, 0xffffffffffffefff) ==
          Kernel::ResultInvalidAddress);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0x1000, 0xfffffffeffffffff) ==
          Kernel::ResultInvalidAddress);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0x8000001, 0x8001000) ==
          Kernel::ResultInvalidAddress);
}

TEST_CASE("NX2 GPU-only regions have a bounded page-aligned size", "[nx2_gsv]") {
    constexpr u64 first = 0x8100000000;
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(first, first + 0x16ad80000 - 1) ==
          ResultSuccess);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(first, first + (8ULL << 30) - 1) ==
          ResultSuccess);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(first, first + (8ULL << 30) + 0xfff) ==
          Kernel::ResultInvalidSize);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(first, first) == Kernel::ResultInvalidSize);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(first, first + 0x1000) ==
          Kernel::ResultInvalidSize);
}

TEST_CASE("NX2 GPU map range validation covers single-byte, aperture, and sparse tile bounds",
          "[nx2_gsv]") {
    // Sub 101/211/213/215 accept single-byte [first, first] and page ranges inside the aperture.
    CHECK(Service::ValidateNx2GpuMapRange(Service::Nx2GpuVaStart, Service::Nx2GpuVaStart) ==
          ResultSuccess);
    CHECK(Service::ValidateNx2GpuMapRange(Service::Nx2GpuVaStart, Service::Nx2GpuVaStart + 0xfff) ==
          ResultSuccess);
    CHECK(Service::ValidateNx2GpuMapRange(Service::Nx2GpuVaEnd - 1, Service::Nx2GpuVaEnd - 1) ==
          ResultSuccess);

    // Backwards, out-of-aperture, and oversized ranges are rejected for sub 211/213/215/216.
    CHECK(Service::ValidateNx2GpuMapRange(Service::Nx2GpuVaStart + 0x1000, Service::Nx2GpuVaStart) ==
          Kernel::ResultInvalidAddress);
    CHECK(Service::ValidateNx2GpuMapRange(0, 0xfff) == Kernel::ResultInvalidAddress);
    CHECK(Service::ValidateNx2GpuMapRange(Service::Nx2GpuVaEnd, Service::Nx2GpuVaEnd + 0xfff) ==
          Kernel::ResultInvalidAddress);
    CHECK(Service::ValidateNx2GpuMapRange(
              Service::Nx2GpuVaStart,
              Service::Nx2GpuVaStart + Service::Nx2MaxGpuOnlyRegionBytes) ==
          Kernel::ResultInvalidSize);
}

TEST_CASE("HeapMapper assigns strictly monotonic IDs immune to pointer-reuse ABA", "[nx2_gsv]") {
    using Service::Nvidia::NvCore::Container;
    using Service::Nvidia::NvCore::HeapMapper;

    const ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    auto host1x = std::make_unique<Tegra::Host1x::Host1x>(system);

    CHECK(Container::IdOf(nullptr) == 0);

    alignas(HeapMapper) std::array<std::byte, sizeof(HeapMapper)> storage{};
    auto* first = std::construct_at(reinterpret_cast<HeapMapper*>(storage.data()), 0x10000000ULL,
                                    0x20000000ULL, 0x1000000ULL, Core::Asid{0}, *host1x);
    const auto first_id = Container::IdOf(first);
    CHECK(first_id != 0);
    CHECK(first->GetId() == first_id);
    std::destroy_at(first);

    auto* second = std::construct_at(reinterpret_cast<HeapMapper*>(storage.data()), 0x10000000ULL,
                                     0x20000000ULL, 0x1000000ULL, Core::Asid{0}, *host1x);
    const auto second_id = Container::IdOf(second);
    REQUIRE(second == first);
    CHECK(second_id > first_id);
    CHECK(second->GetId() == second_id);
    std::destroy_at(second);
}

TEST_CASE("HeapMapper RangeInBounds and IsInBounds reject overflow and out-of-range spans",
          "[nx2_gsv][nvdrv]") {
    using Service::Nvidia::NvCore::HeapMapper;

    constexpr VAddr base = 0x10000000ULL;
    constexpr size_t region_size = 0x1000000ULL;

    // Exact match, interior subrange, and zero-size boundary anchors.
    CHECK(HeapMapper::RangeInBounds(base, region_size, base, region_size));
    CHECK(HeapMapper::RangeInBounds(base + 0x1000, 0x2000, base, region_size));
    CHECK(HeapMapper::RangeInBounds(base, 0, base, region_size));
    CHECK(HeapMapper::RangeInBounds(base + region_size, 0, base, region_size));

    // Below base and past base + region_size.
    CHECK_FALSE(HeapMapper::RangeInBounds(base - 0x1000, 0x2000, base, region_size));
    CHECK_FALSE(HeapMapper::RangeInBounds(base - 0x2000, 0x1000, base, region_size));
    CHECK_FALSE(HeapMapper::RangeInBounds(base - 1, 0, base, region_size));
    CHECK_FALSE(
        HeapMapper::RangeInBounds(base + region_size - 0x1000, 0x2000, base, region_size));
    CHECK_FALSE(HeapMapper::RangeInBounds(base + region_size, 0x1000, base, region_size));
    CHECK_FALSE(HeapMapper::RangeInBounds(base, region_size + 1, base, region_size));
    CHECK_FALSE(HeapMapper::RangeInBounds(base + region_size + 1, 0, base, region_size));

    // Wrapping sizes where start + size would wrap below start (and below base + region_size).
    CHECK_FALSE(HeapMapper::RangeInBounds(0x10000000ULL,
                                          std::numeric_limits<size_t>::max() - 0x1000ULL, base,
                                          region_size));
    CHECK_FALSE(HeapMapper::RangeInBounds(std::numeric_limits<VAddr>::max() - 0x100ULL, 0x200ULL,
                                          base, region_size));
    CHECK_FALSE(HeapMapper::RangeInBounds(std::numeric_limits<VAddr>::max() - 0x100ULL, 0x200ULL,
                                          std::numeric_limits<VAddr>::max() - 0x1000ULL,
                                          0x1000ULL));

    const ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    auto host1x = std::make_unique<Tegra::Host1x::Host1x>(system);
    const HeapMapper mapper{base, 0x20000000ULL, region_size, Core::Asid{0}, *host1x};

    CHECK(mapper.IsInBounds(base, region_size));
    CHECK(mapper.IsInBounds(base + 0x1000, 0x2000));
    CHECK(mapper.IsInBounds(base, 0));
    CHECK(mapper.IsInBounds(base + region_size, 0));
    CHECK_FALSE(mapper.IsInBounds(base - 0x1000, 0x2000));
    CHECK_FALSE(mapper.IsInBounds(base + region_size - 0x1000, 0x2000));
    CHECK_FALSE(mapper.IsInBounds(base + region_size, 0x1000));
    CHECK_FALSE(
        mapper.IsInBounds(0x10000000ULL, std::numeric_limits<size_t>::max() - 0x1000ULL));
    CHECK_FALSE(mapper.IsInBounds(std::numeric_limits<VAddr>::max() - 0x100ULL, 0x200ULL));
}

TEST_CASE("NvCore Container guards out-of-bounds sessions and stale HeapMapper IDs",
          "[nx2_gsv][nvdrv]") {
    using Service::Nvidia::NvCore::Container;
    using Service::Nvidia::NvCore::HeapMapper;
    using Service::Nvidia::NvCore::SessionId;

    NvdrvTestHarness harness;
    Container& container = *harness.container;

    CHECK(container.GetSession(SessionId{999}) == nullptr);
    container.CloseSession(SessionId{999});

    size_t callbacks = 0;
    container.WithSessionMapper(SessionId{999}, 1, [&](HeapMapper&) { ++callbacks; });
    CHECK(callbacks == 0);

    const auto sid = container.OpenSession(harness.process->GetHandle());
    auto* session = container.GetSession(sid);
    REQUIRE(session != nullptr);

    container.WithSessionMapper(sid, 0, [&](HeapMapper&) { ++callbacks; });
    CHECK(callbacks == 0);

    constexpr VAddr mapper_vaddr = 0x10000000ULL;
    constexpr size_t mapper_size = 0x1000000ULL;
    const DAddr mapper_daddr = harness.host1x->MemoryManager().Allocate(mapper_size);
    REQUIRE(mapper_daddr != 0);

    session->mapper = std::make_unique<HeapMapper>(mapper_vaddr, mapper_daddr, mapper_size,
                                                   session->asid, *harness.host1x);
    session->has_preallocated_area = true;
    const auto mapper_id = Container::IdOf(session->mapper.get());
    REQUIRE(mapper_id != 0);

    HeapMapper* seen_mapper = nullptr;
    container.WithSessionMapper(sid, mapper_id, [&](HeapMapper& mapper) {
        ++callbacks;
        seen_mapper = &mapper;
    });
    CHECK(callbacks == 1);
    CHECK(seen_mapper == session->mapper.get());

    container.WithSessionMapper(sid, mapper_id + 1, [&](HeapMapper&) { ++callbacks; });
    CHECK(callbacks == 1);

    container.CloseSession(sid);
    CHECK(container.GetSession(sid) == nullptr);

    container.WithSessionMapper(sid, mapper_id, [&](HeapMapper&) { ++callbacks; });
    CHECK(callbacks == 1);

    // Reopening reuses the session slot ID, but the old HeapMapper ID remains rejected.
    const auto reopened_sid = container.OpenSession(harness.process->GetHandle());
    CHECK(reopened_sid.id == sid.id);
    container.WithSessionMapper(reopened_sid, mapper_id, [&](HeapMapper&) { ++callbacks; });
    CHECK(callbacks == 1);
    container.CloseSession(reopened_sid);
}

TEST_CASE("NvMap rejects zero-size creation, invalid pin sessions, and unmaps on session close",
          "[nx2_gsv][nvdrv]") {
    using Service::Nvidia::NvResult;
    using Service::Nvidia::NvCore::Container;
    using Service::Nvidia::NvCore::NvMap;
    using Service::Nvidia::NvCore::SessionId;

    NvdrvTestHarness harness;
    Container& container = *harness.container;
    NvMap& nvmap = container.GetNvMapFile();

    std::shared_ptr<NvMap::Handle> zero_handle;
    CHECK(nvmap.CreateHandle(0, zero_handle) == NvResult::BadValue);
    CHECK(zero_handle == nullptr);

    std::shared_ptr<NvMap::Handle> bogus_handle;
    REQUIRE(nvmap.CreateHandle(0x1000, bogus_handle) == NvResult::Success);
    REQUIRE(bogus_handle != nullptr);
    CHECK(bogus_handle->Duplicate(false) == NvResult::BadValue);

    CHECK(bogus_handle->Alloc({}, 0x1000, 0, 0x10000000ULL, SessionId{42}) == NvResult::Success);
    CHECK(nvmap.PinHandle(bogus_handle->id, false) == 0);
    CHECK(bogus_handle->pins == 0);

    const auto sid = container.OpenSession(harness.process->GetHandle());
    std::shared_ptr<NvMap::Handle> session_handle;
    REQUIRE(nvmap.CreateHandle(0x1000, session_handle) == NvResult::Success);
    REQUIRE(session_handle != nullptr);
    const auto session_handle_id = session_handle->id;

    CHECK(session_handle->Alloc({}, 0x1000, 0, 0x20000000ULL, sid) == NvResult::Success);
    CHECK(session_handle->Alloc({}, 0x1000, 0, 0x30000000ULL, sid) == NvResult::AccessDenied);

    container.CloseSession(sid);
    CHECK(nvmap.GetHandle(session_handle_id) == nullptr);
    CHECK(nvmap.PinHandle(session_handle_id, false) == 0);
}

TEST_CASE("DeviceMemoryManager ignores Map and UnregisterProcess for invalid or unregistered ASIDs",
          "[nx2_gsv][nvdrv]") {
    const ScopedSystemTestContext context;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager dmm{physical};

    dmm.UnregisterProcess(Core::Asid{99});
    dmm.Map(0x100000, 0x200000, 0x1000, Core::Asid{99});
    CHECK(dmm.GetPointer<u8>(0x100000) == nullptr);

    const auto asid = dmm.RegisterProcess(nullptr);
    dmm.UnregisterProcess(asid);
    dmm.Map(0x100000, 0x200000, 0x1000, asid);
    CHECK(dmm.GetPointer<u8>(0x100000) == nullptr);

    const auto reused_asid = dmm.RegisterProcess(nullptr);
    CHECK(reused_asid.id == asid.id);
}
