// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <limits>
#include <memory>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "core/core.h"
#include "core/file_sys/program_metadata.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/heap_mapper.h"
#include "core/hle/service/nx2_gsv.h"
#include "tests/core/system_test_context.h"
#include "video_core/host1x/host1x.h"

TEST_CASE("NX2 GPU map ranges stay inside the GPU address window", "[nx2_guest_ranges]") {
    // A range inside the window is usable.
    CHECK(Service::ValidateNx2GpuMapRange(Service::Nx2GpuVaStart,
                                          Service::Nx2GpuVaStart + 0xfff) == ResultSuccess);
    CHECK(Service::ValidateNx2GpuMapRange(Service::Nx2GpuVaEnd - 1, Service::Nx2GpuVaEnd - 1) ==
          ResultSuccess);

    // The rule the map job used: the last address is above the first one, and nothing else. The
    // size it computed (last - first + 1) then wrapped around the address space.
    const auto old_rule = [](u64 first, u64 last) { return last > first; };
    REQUIRE(old_rule(1, std::numeric_limits<u64>::max()));
    CHECK(Service::ValidateNx2GpuMapRange(1, std::numeric_limits<u64>::max()) ==
          Kernel::ResultInvalidAddress);

    // A range that starts below the window, or that reaches past its end, is not inside it.
    CHECK(Service::ValidateNx2GpuMapRange(Service::Nx2GpuVaStart - 0x1000,
                                          Service::Nx2GpuVaStart - 1) ==
          Kernel::ResultInvalidAddress);
    CHECK(Service::ValidateNx2GpuMapRange(Service::Nx2GpuVaEnd, Service::Nx2GpuVaEnd + 0xfff) ==
          Kernel::ResultInvalidAddress);
    CHECK(Service::ValidateNx2GpuMapRange(Service::Nx2GpuVaEnd - 0x1000, Service::Nx2GpuVaEnd) ==
          Kernel::ResultInvalidAddress);

    // A range that runs backwards has no size at all.
    CHECK(Service::ValidateNx2GpuMapRange(Service::Nx2GpuVaStart + 0x1000, Service::Nx2GpuVaStart) ==
          Kernel::ResultInvalidAddress);
}

TEST_CASE("NX2 GPU map ranges stay inside the per-region cap", "[nx2_guest_ranges]") {
    constexpr u64 first = Service::Nx2GpuVaStart;
    CHECK(Service::ValidateNx2GpuMapRange(first, first + Service::Nx2MaxGpuOnlyRegionBytes - 1) ==
          ResultSuccess);
    CHECK(Service::ValidateNx2GpuMapRange(first, first + Service::Nx2MaxGpuOnlyRegionBytes) ==
          Kernel::ResultInvalidSize);
    // A range that is too big for the cap cannot have a size that wraps the big-page rounding
    // either: the cap is below the point where that happens.
    CHECK(Service::ValidateNx2GpuMapRange(1, std::numeric_limits<u64>::max()) ==
          Kernel::ResultInvalidAddress);
}

TEST_CASE("NX2 heap mapper bounds reject a range that wraps", "[nx2_guest_ranges]") {
    using Service::Nvidia::NvCore::HeapMapper;
    constexpr VAddr base = 0x10000000;
    constexpr size_t region_size = 0x1000000;

    // A range inside the heap is fine.
    CHECK(HeapMapper::RangeInBounds(base, 0x1000, base, region_size));
    CHECK(HeapMapper::RangeInBounds(base + region_size - 0x1000, 0x1000, base, region_size));

    // A range that starts before the heap, or that ends past it, is not.
    CHECK_FALSE(HeapMapper::RangeInBounds(base - 0x1000, 0x1000, base, region_size));
    CHECK_FALSE(HeapMapper::RangeInBounds(base + region_size, 0x1000, base, region_size));

    // A size that makes the end of the range wrap below its start is not inside the heap, even
    // though the address it wraps to is.
    CHECK_FALSE(HeapMapper::RangeInBounds(base + 0x1000, std::numeric_limits<size_t>::max(), base,
                                          region_size));
    CHECK_FALSE(HeapMapper::RangeInBounds(base, std::numeric_limits<size_t>::max(), base,
                                          region_size));
}

TEST_CASE("NX2 image pool sizes stay inside the pool budget", "[nx2_guest_ranges]") {
    // A pool of 4 MiB images: the count that fills the budget is the budget over the image size.
    const auto pool = Service::Nx2ImagePoolBufferSize(0x400000, 1024);
    REQUIRE(pool);
    CHECK(*pool == Service::Nx2MaxImagePoolBytes);

    // One image too many is refused instead of sizing the pool past its budget.
    CHECK_FALSE(Service::Nx2ImagePoolBufferSize(0x400000, 1025));

    // A count whose product with the image size wrapped around is refused as well: the guest
    // would be told to allocate a buffer that is far too small.
    CHECK_FALSE(Service::Nx2ImagePoolBufferSize(1ULL << 34, std::numeric_limits<u32>::max()));

    // A count of zero means one image, like the pool creation does.
    const auto single = Service::Nx2ImagePoolBufferSize(0x400000, 0);
    REQUIRE(single);
    CHECK(*single == 0x400000);
}

TEST_CASE("The kernel keeps its object list container after the shutdown", "[nx2_guest_ranges]") {
    const ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    kernel.Initialize();

    const auto objects_before = kernel.ObjectListContainer().GetOwnedCount(nullptr);
    kernel.Shutdown();
    // The container stays alive after the shutdown: an object closed later still finds it and
    // unregisters itself from it. The list itself is emptied, because the objects in it live
    // in the guest RAM arena that the system frees before it destroys the kernel.
    CHECK(kernel.ObjectListContainer().GetOwnedCount(nullptr) == 0);
    CHECK(objects_before > 0);
}

TEST_CASE("Nvidia Container::WithSession safely guards active sessions across close races",
          "[nx2_guest_ranges]") {
    const ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    kernel.Initialize();
    SCOPE_EXIT {
        system.ShutdownMainProcess();
    };

    Kernel::KProcess* const process = Kernel::KProcess::Create(kernel);
    REQUIRE(process != nullptr);
    Kernel::KProcess::Register(kernel, process);
    SCOPE_EXIT {
        process->Close(kernel);
    };
    const auto metadata = FileSys::ProgramMetadata::GetDefault();
    REQUIRE(process->LoadFromMetadata(kernel, metadata, Kernel::PageSize, {}, 0).IsSuccess());

    auto host1x = std::make_unique<Tegra::Host1x::Host1x>(system);
    Service::Nvidia::NvCore::Container container{*host1x};

    bool invalid_called = false;
    CHECK_FALSE(container.WithSession(Service::Nvidia::NvCore::SessionId{42},
                                      [&](Service::Nvidia::NvCore::Session&) { invalid_called = true; }));
    CHECK_FALSE(invalid_called);

    const auto session_id = container.OpenSession(process);
    Kernel::KProcess* observed_process = nullptr;
    REQUIRE(container.WithSession(session_id, [&](Service::Nvidia::NvCore::Session& session) {
        observed_process = session.process;
    }));
    CHECK(observed_process == process);

    container.CloseSession(session_id);
    bool closed_called = false;
    CHECK_FALSE(container.WithSession(session_id,
                                      [&](Service::Nvidia::NvCore::Session&) { closed_called = true; }));
    CHECK_FALSE(closed_called);

    std::atomic<bool> stop{false};
    std::thread churn([&] {
        for (int i = 0; i < 64; ++i) {
            const auto id = container.OpenSession(process);
            std::this_thread::yield();
            container.CloseSession(id);
        }
        stop.store(true, std::memory_order_release);
    });

    std::atomic<bool> mismatch{false};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            container.WithSession(Service::Nvidia::NvCore::SessionId{0},
                                  [&](Service::Nvidia::NvCore::Session& s) {
                                      if (s.process != process || !s.is_active) {
                                          mismatch.store(true, std::memory_order_relaxed);
                                      }
                                  });
        }
    });

    churn.join();
    reader.join();
    CHECK_FALSE(mismatch.load(std::memory_order_relaxed));
}
