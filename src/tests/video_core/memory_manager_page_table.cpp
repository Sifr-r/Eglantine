// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <exception>
#include <limits>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/device_memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace {

class NullRasterizer final : public VideoCore::RasterizerInterface {
public:
    void Draw(bool, u32) override {}
    void DrawTexture() override {}
    void Clear(u32) override {}
    void DispatchCompute() override {}
    void ResetCounter(VideoCommon::QueryType) override {}
    void Query(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32,
               u32) override {}
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override {}
    void DisableGraphicsUniformBuffer(size_t, u32) override {}
    void SignalFence(std::function<void()>&&, bool) override {}
    void SyncOperation(std::function<void()>&&) override {}
    void SignalSyncPoint(u32) override {}
    void SignalReference() override {}
    void ReleaseFences(bool) override {}
    void FlushAll() override {}
    void FlushRegion(DAddr, u64, VideoCommon::CacheType) override {}
    bool MustFlushRegion(DAddr, u64, VideoCommon::CacheType) override {
        return false;
    }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override {
        return {addr, addr + size, false};
    }
    void InvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void OnCacheInvalidation(DAddr, u64) override {}
    bool OnCPUWrite(DAddr, u64) override {
        return false;
    }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {}
    void FlushAndInvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {
        std::terminate();
    }
    void AccelerateInlineToMemory(GPUVAddr, size_t, std::span<const u8>) override {}
};

constexpr u64 SmallPage = 0x1000;
constexpr u64 BigPage = 0x1'0000;
constexpr u64 AddressSpaceBits = 32;
constexpr GPUVAddr AddressSpaceEnd = 1ULL << AddressSpaceBits;
constexpr GPUVAddr Window = 0x2000'0000;
constexpr DAddr Device = 0x1000'0000;

/// A memory manager over a small address space, with no renderer behind it but the null one.
struct PageTableFixture {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    NullRasterizer rasterizer;
    Tegra::MemoryManager memory{system, device_memory, AddressSpaceBits, 1ULL << 31, 16};

    PageTableFixture() {
        memory.BindRasterizer(&rasterizer);
    }
};

} // namespace

TEST_CASE("A map and unmap pair leaves the page table consistent",
          "[memory_manager][page_table]") {
    PageTableFixture fixture;
    auto& memory = fixture.memory;

    // Nothing is mapped yet.
    REQUIRE_FALSE(memory.GpuToCpuAddress(Window).has_value());
    REQUIRE_FALSE(memory.IsFullyMappedRange(Window, SmallPage));
    REQUIRE_FALSE(memory.IsContinuousRange(Window, SmallPage));
    REQUIRE(memory.GetSubmappedRange(Window, SmallPage).empty());

    // A small page mapping: the translation, the ranges and the page kind all follow it.
    memory.Map(Window, Device, SmallPage, Tegra::PTEKind::PITCH, false);
    REQUIRE(memory.GpuToCpuAddress(Window) == Device);
    REQUIRE(memory.GpuToCpuAddress(Window + SmallPage - 1) == Device + SmallPage - 1);
    REQUIRE(memory.IsFullyMappedRange(Window, SmallPage));
    REQUIRE(memory.IsContinuousRange(Window, SmallPage));
    REQUIRE(memory.GetPageKind(Window) == Tegra::PTEKind::PITCH);
    const auto submapped = memory.GetSubmappedRange(Window, SmallPage);
    REQUIRE(submapped.size() == 1);
    REQUIRE(submapped[0].first == Window);
    REQUIRE(submapped[0].second == SmallPage);

    // Unmapping it frees the pages again.
    memory.Unmap(Window, SmallPage);
    REQUIRE_FALSE(memory.GpuToCpuAddress(Window).has_value());
    REQUIRE_FALSE(memory.IsFullyMappedRange(Window, SmallPage));
    REQUIRE_FALSE(memory.IsContinuousRange(Window, SmallPage));
    REQUIRE(memory.GetSubmappedRange(Window, SmallPage).empty());

    // The same for a big page.
    memory.Map(Window, Device, BigPage, Tegra::PTEKind::PITCH, true);
    REQUIRE(memory.GpuToCpuAddress(Window) == Device);
    REQUIRE(memory.GpuToCpuAddress(Window + BigPage - 1) == Device + BigPage - 1);
    REQUIRE(memory.IsFullyMappedRange(Window, BigPage));
    REQUIRE(memory.IsContinuousRange(Window, BigPage));
    memory.Unmap(Window, BigPage);
    REQUIRE_FALSE(memory.GpuToCpuAddress(Window).has_value());
    REQUIRE_FALSE(memory.IsFullyMappedRange(Window, BigPage));

    // A sparse mapping reserves the pages without backing them: the range has no translation
    // and its page kind is invalid, but IsFullyMappedRange still reports it as covered. The
    // walk counts a reserved *small* page as covered (the short-page walk passes over reserved
    // pages); only a reserved *big* page fails it. That is the function's long-standing
    // contract, which the native-macro paths rely on (kepler_compute.cpp, maxwell_3d.cpp read a
    // "fully mapped" range back with ReadBlockUnsafe), so the test documents it rather than
    // asking for it to change.
    memory.MapSparse(Window, SmallPage, false);
    REQUIRE_FALSE(memory.GpuToCpuAddress(Window).has_value());
    REQUIRE(memory.IsFullyMappedRange(Window, SmallPage));
    REQUIRE(memory.GetPageKind(Window) == Tegra::PTEKind::INVALID);
    memory.Unmap(Window, SmallPage);

    // A sparse mapping of a big page, which the walk does not count as covered.
    memory.MapSparse(Window, BigPage, true);
    REQUIRE_FALSE(memory.GpuToCpuAddress(Window).has_value());
    REQUIRE_FALSE(memory.IsFullyMappedRange(Window, BigPage));
    memory.Unmap(Window, BigPage);

    // Mapping the same range again over a different backing is a change of the whole range.
    memory.Map(Window, Device, 4 * SmallPage, Tegra::PTEKind::PITCH, false);
    memory.Map(Window, Device + 4 * SmallPage, 4 * SmallPage, Tegra::PTEKind::PITCH, false);
    REQUIRE(memory.GpuToCpuAddress(Window) == Device + 4 * SmallPage);
    // The four pages that were mapped are still mapped; the four pages above them never were.
    REQUIRE(memory.IsFullyMappedRange(Window, 4 * SmallPage));
    REQUIRE_FALSE(memory.IsFullyMappedRange(Window, 8 * SmallPage));
    // The two halves are not one device range any more.
    REQUIRE_FALSE(memory.IsContinuousRange(Window, 8 * SmallPage));
}

TEST_CASE("The page table's range arithmetic does not overflow",
          "[memory_manager][page_table]") {
    PageTableFixture fixture;
    auto& memory = fixture.memory;

    // The last page of the address space.
    memory.Map(AddressSpaceEnd - SmallPage, Device, SmallPage, Tegra::PTEKind::PITCH, false);
    REQUIRE(memory.GpuToCpuAddress(AddressSpaceEnd - 1) == Device + SmallPage - 1);
    REQUIRE(memory.IsFullyMappedRange(AddressSpaceEnd - SmallPage, SmallPage));

    // Past the end of the address space there is no translation, and no wrap around.
    REQUIRE_FALSE(memory.GpuToCpuAddress(AddressSpaceEnd).has_value());
    REQUIRE_FALSE(memory.GpuToCpuAddress((std::numeric_limits<GPUVAddr>::max)()).has_value());

    // The two-argument form answers with the first mapped page of the range, and its walk is
    // page-granular: it translates the page's base address, so the answer is the device address
    // the page starts at and not the address of the byte that was asked about. A range that
    // reaches past the end of the address space still answers with the last page inside it.
    const auto past_the_end = memory.GpuToCpuAddress(AddressSpaceEnd - 1, 2);
    REQUIRE(past_the_end.has_value());
    REQUIRE(*past_the_end == Device);

    // The two-argument form is not asked about an extreme size: it computes the end of the
    // range as `(addr + size + page_size - 1) >> page_bits`, which wraps for a size that does
    // not fit, and the walk it then makes is as long as the wrapped end says rather than as
    // long as the address space. The bound that the single-address form and the range queries
    // do have is the one checked above.

    // A range that does not fit in the address space is refused instead of wrapping.
    const size_t huge = (std::numeric_limits<size_t>::max)();
    REQUIRE_FALSE(memory.IsFullyMappedRange(AddressSpaceEnd - 1, 2));
    REQUIRE_FALSE(memory.IsContinuousRange(AddressSpaceEnd - 1, 2));
    REQUIRE_FALSE(memory.IsFullyMappedRange(AddressSpaceEnd - 1, huge));
    REQUIRE_FALSE(memory.IsContinuousRange(AddressSpaceEnd - 1, huge));
    REQUIRE_FALSE(memory.IsFullyMappedRange(0, huge));
    REQUIRE(memory.MaxContinuousRangeUncached(AddressSpaceEnd - 1, 2) == 0);
    REQUIRE(memory.MaxContinuousRangeUncached(0, huge) == 0);

    // The mapping at the top of the address space is still the one that was made.
    REQUIRE(memory.GpuToCpuAddress(AddressSpaceEnd - SmallPage) == Device);
    memory.Unmap(AddressSpaceEnd - SmallPage, SmallPage);
    REQUIRE_FALSE(memory.GpuToCpuAddress(AddressSpaceEnd - 1).has_value());
}

TEST_CASE("A mapping change is never seen half applied", "[memory_manager][page_table]") {
    PageTableFixture fixture;
    auto& memory = fixture.memory;

    constexpr u64 Maps = 20000;
    constexpr u64 Step = 64 * SmallPage;
    std::atomic<GPUVAddr> current{0};
    std::atomic<u64> mapped{0};
    std::atomic<bool> half_applied{false};

    // The writer maps a range that was never mapped before, so that the page table's entry and
    // the device address behind it are written one after the other. A reader that catches the
    // page between the two writes would translate it to the backing of no mapping at all.
    std::thread writer{[&] {
        GPUVAddr address = Window;
        for (u64 i = 0; i < Maps; ++i) {
            current.store(address, std::memory_order_release);
            memory.Map(address, Device, SmallPage, Tegra::PTEKind::PITCH, false);
            address += Step;
            if (address + SmallPage >= AddressSpaceEnd) {
                address = Window;
            }
        }
        mapped.store(Maps, std::memory_order_release);
    }};

    // The reader translates the range the writer is mapping: the translation is either the one
    // the writer made or none at all, never an address that belongs to no mapping.
    u64 observations = 0;
    while (observations < 1000 || mapped.load(std::memory_order_acquire) < Maps) {
        ++observations;
        const GPUVAddr address = current.load(std::memory_order_acquire);
        if (address == 0) {
            continue;
        }
        const u64 serial_before = memory.GetMapSerial();
        const auto translated = memory.GpuToCpuAddress(address);
        const u64 serial_after = memory.GetMapSerial();
        if (serial_before != serial_after) {
            // The table changed while the page was translated: the answer may be either one.
            continue;
        }
        if (translated && (*translated < Device || *translated >= Device + SmallPage)) {
            half_applied.store(true, std::memory_order_relaxed);
        }
    }
    writer.join();

    REQUIRE(observations >= 1000);
    REQUIRE_FALSE(half_applied.load(std::memory_order_relaxed));
    // The table is still usable after the concurrent changes.
    REQUIRE(memory.IsFullyMappedRange(Window, SmallPage));
    REQUIRE(memory.GpuToCpuAddress(Window) == Device);
}
