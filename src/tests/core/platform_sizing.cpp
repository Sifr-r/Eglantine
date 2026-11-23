// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "common/address_space.h"
#include "common/page_table.h"
#include "common/scope_exit.h"
#include "common/scratch_buffer.h"
#include "common/settings.h"
#include "common/sparse_large_vector.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/device_memory.h"
#include "core/device_memory_manager.h"
#include "core/file_sys/program_metadata.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/global_scheduler_context.h"
#include "core/hle/kernel/k_handle_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/svc_common.h"
#include "core/hle/kernel/svc_results.h"
#include "core/memory.h"
#include "core/platform_profile.h"
#include "tests/core/system_test_context.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace {

class TestRasterizer final : public VideoCore::RasterizerInterface {
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
    void OnCacheInvalidation(PAddr, u64) override {}
    bool OnCPUWrite(PAddr, u64) override {
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

void VerifySingleCoreRotation(Core::CpuManager& cpu_manager, u32 active_cores) {
    const u64 expected_mask = (u64{1} << active_cores) - 1;
    u64 seen_mask = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (seen_mask != expected_mask && std::chrono::steady_clock::now() < deadline) {
        const std::size_t core = cpu_manager.CurrentCore();
        REQUIRE(core < active_cores);
        seen_mask |= (u64{1} << core);
        std::this_thread::yield();
    }
    REQUIRE(seen_mask == expected_mask);
}

void RunLifecycleCycle(Core::System& system, u32 active_cores, bool multicore) {
    auto& kernel = system.Kernel();
    auto& cpu_manager = system.GetCpuManager();

    kernel.SetMulticore(multicore);
    cpu_manager.SetMulticore(multicore);
    cpu_manager.SetAsyncGpu(true);
    kernel.SetActiveCoreCount(active_cores);
    cpu_manager.SetActiveCoreCount(active_cores);
    REQUIRE(kernel.ActiveCoreCount() == active_cores);
    REQUIRE(cpu_manager.ActiveCoreCount() == active_cores);

    kernel.Initialize();
    for (u32 core = 0; core < active_cores; ++core) {
        CHECK(kernel.Scheduler(core).GetIdleThread() != nullptr);
        CHECK(kernel.Scheduler(core).GetSchedulerCurrentThread() != nullptr);
        CHECK(kernel.PhysicalCore(core).CoreIndex() == core);
    }

    if (multicore) {
        system.CoreTiming().SyncPause(false);
        REQUIRE(system.CoreTiming().IsRunning());
    }

    cpu_manager.Initialize();
    cpu_manager.OnGpuReady();

    if (!multicore) {
        VerifySingleCoreRotation(cpu_manager, active_cores);
    }

    system.Pause();
    CHECK(system.IsPaused());
    if (multicore) {
        CHECK_FALSE(system.CoreTiming().IsRunning());
    }

    system.Run();
    CHECK_FALSE(system.IsPaused());
    if (multicore) {
        CHECK(system.CoreTiming().IsRunning());
    } else {
        VerifySingleCoreRotation(cpu_manager, active_cores);
    }

    system.ShutdownMainProcess();
    CHECK(kernel.ObjectListContainer().GetOwnedCount(nullptr) == 0);
    if (multicore) {
        CHECK_FALSE(system.CoreTiming().IsRunning());
    }
}

} // namespace

TEST_CASE("CpuManager and KernelCore single-core and multi-core lifecycles respect active core count",
          "[core][platform-sizing]") {
    const ScopedSystemTestContext context;
    const bool prev_async_gpu = Settings::values.use_asynchronous_gpu_emulation.GetValue();
    SCOPE_EXIT {
        Settings::values.use_asynchronous_gpu_emulation.SetValue(prev_async_gpu);
    };
    Settings::values.use_asynchronous_gpu_emulation.SetValue(true);

    const bool multicore = GENERATE(false, true);
    const u32 active_cores = GENERATE(u32{4}, u32{6});
    CAPTURE(multicore, active_cores);

    Settings::values.use_multi_core.SetValue(multicore);
    Core::System system;
    system.Initialize();
    RunLifecycleCycle(system, active_cores, multicore);
}

TEST_CASE("CpuManager and KernelCore cleanly transition from 6-core Switch 2 to 4-core Switch 1",
          "[core][platform-sizing]") {
    const ScopedSystemTestContext context;
    const bool prev_async_gpu = Settings::values.use_asynchronous_gpu_emulation.GetValue();
    SCOPE_EXIT {
        Settings::values.use_asynchronous_gpu_emulation.SetValue(prev_async_gpu);
    };
    Settings::values.use_asynchronous_gpu_emulation.SetValue(true);
    Settings::values.use_multi_core.SetValue(true);

    Core::System system;
    system.Initialize();
    RunLifecycleCycle(system, 6, true);
    system.SetShuttingDown(false);
    RunLifecycleCycle(system, 4, true);

    system.Kernel().SetActiveCoreCount(0);
    CHECK(system.Kernel().ActiveCoreCount() == 1);
    system.Kernel().SetActiveCoreCount(Core::Hardware::NUM_CPU_CORES + 4);
    CHECK(system.Kernel().ActiveCoreCount() == Core::Hardware::NUM_CPU_CORES);

    system.GetCpuManager().SetActiveCoreCount(0);
    CHECK(system.GetCpuManager().ActiveCoreCount() == 1);
    system.GetCpuManager().SetActiveCoreCount(Core::Hardware::NUM_CPU_CORES + 4);
    CHECK(system.GetCpuManager().ActiveCoreCount() == Core::Hardware::NUM_CPU_CORES);
}

TEST_CASE("SVC thread core validation enforces Switch 1 4-core limits and Switch 2 6-core folding",
          "[core][platform-sizing]") {
    const ScopedSystemTestContext context;
    const bool is_switch2 = GENERATE(false, true);
    const u32 active_cores = is_switch2 ? 6U : 4U;
    CAPTURE(is_switch2, active_cores);

    Core::System system;
    system.Initialize();
    system.SetPlatformProfile(Core::MakePlatformProfile(
        is_switch2 ? Core::PlatformResolution::ConfirmedSwitch2
                   : Core::PlatformResolution::ConfirmedSwitch1));
    auto& kernel = system.Kernel();
    kernel.SetActiveCoreCount(active_cores);
    kernel.Initialize();
    SCOPE_EXIT {
        system.ShutdownMainProcess();
    };

    auto* const process = Kernel::KProcess::Create(kernel);
    REQUIRE(process != nullptr);
    Kernel::KProcess::Register(kernel, process);
    SCOPE_EXIT {
        process->Close(kernel);
    };

    // Allow cores 0..7 in capabilities so Switch 1 core limit enforcement is tested independently
    // of NPDM core-mask trimming.
    FileSys::ProgramMetadata metadata;
    metadata.LoadManual(true, FileSys::ProgramAddressSpaceType::Is39Bit, 0x2c, 0, 0x100000,
                        0x0100123400001000ULL, 0xFFFFFFFFFFFFFFFFULL, 0,
                        {0x070043F7U, (64U << 16) | 0x7FFFU});
    REQUIRE(process->LoadFromMetadata(kernel, metadata, Kernel::PageSize, {}, 0).IsSuccess());
    process->GetMemory().SetCurrentPageTable(*process);
    REQUIRE(process->GetHandleTable().Initialize(kernel, 64).IsSuccess());
    SCOPE_EXIT {
        process->GetHandleTable().Finalize(kernel);
    };

    Kernel::KScopedResourceReservation owner_res(
        kernel, process, Kernel::Svc::LimitableResource::ThreadCountMax);
    REQUIRE(owner_res.Succeeded());
    Kernel::KThread* const owner = Kernel::KThread::Create(kernel);
    REQUIRE(owner != nullptr);
    REQUIRE(Kernel::KThread::InitializeUserThread(system, owner, {}, 0, 0x100000, 0x2c, 0, process)
                .IsSuccess());
    Kernel::KThread::Register(kernel, owner);
    owner_res.Commit();
    SCOPE_EXIT {
        kernel.GlobalSchedulerContext().RemoveThread(owner);
        owner->Close(kernel);
    };

    auto* const original_thread = Kernel::GetCurrentThreadPointer(kernel);
    Kernel::SetCurrentThread(kernel, owner);
    owner->EnableDispatch(kernel);
    SCOPE_EXIT {
        owner->DisableDispatch(kernel);
        Kernel::SetCurrentThread(kernel, original_thread);
    };

    auto close_svc_thread = [&](Kernel::Handle handle) {
        if (handle == Kernel::Svc::InvalidHandle) {
            return;
        }
        if (auto obj = process->GetHandleTable().GetObject<Kernel::KThread>(kernel, handle);
            obj.IsNotNull()) {
            kernel.GlobalSchedulerContext().RemoveThread(obj.GetPointerUnsafe());
        }
        CHECK(Kernel::Svc::CloseHandle64(system, handle).IsSuccess());
    };

    if (!is_switch2) {
        Kernel::Handle valid_handle = Kernel::Svc::InvalidHandle;
        REQUIRE(Kernel::Svc::CreateThread64(system, &valid_handle, 0x100000, 0, 0x200000, 0x2c, 3)
                    .IsSuccess());
        SCOPE_EXIT {
            close_svc_thread(valid_handle);
        };

        for (const s32 bad_core : {4, 5, 6, 7}) {
            Kernel::Handle bad_handle = Kernel::Svc::InvalidHandle;
            CHECK(Kernel::Svc::CreateThread64(system, &bad_handle, 0x100000, 0, 0x200000, 0x2c,
                                              bad_core) == Kernel::ResultInvalidCoreId);
            CHECK(bad_handle == Kernel::Svc::InvalidHandle);
        }

        CHECK(Kernel::Svc::SetThreadCoreMask64(system, valid_handle, 4, u64{1} << 4) ==
              Kernel::ResultInvalidCoreId);
        CHECK(Kernel::Svc::SetThreadCoreMask64(system, valid_handle, 6, u64{1} << 6) ==
              Kernel::ResultInvalidCoreId);
        CHECK(Kernel::Svc::SetThreadCoreMask64(system, valid_handle, 2, u64{1} << 2).IsSuccess());
    } else {
        Kernel::Handle core5_handle = Kernel::Svc::InvalidHandle;
        REQUIRE(Kernel::Svc::CreateThread64(system, &core5_handle, 0x100000, 0, 0x200000, 0x2c, 5)
                    .IsSuccess());
        SCOPE_EXIT {
            close_svc_thread(core5_handle);
        };

        Kernel::Handle folded_handle = Kernel::Svc::InvalidHandle;
        REQUIRE(Kernel::Svc::CreateThread64(system, &folded_handle, 0x100000, 0, 0x200000, 0x2c, 6)
                    .IsSuccess());
        SCOPE_EXIT {
            close_svc_thread(folded_handle);
        };

        s32 out_core = -1;
        u64 out_mask = 0;
        REQUIRE(Kernel::Svc::GetThreadCoreMask64(system, &out_core, &out_mask, folded_handle)
                    .IsSuccess());
        CHECK(out_core == 0);
        CHECK(out_mask == (u64{1} << 0));

        REQUIRE(
            Kernel::Svc::SetThreadCoreMask64(system, folded_handle, 7, u64{1} << 7).IsSuccess());
        REQUIRE(Kernel::Svc::GetThreadCoreMask64(system, &out_core, &out_mask, folded_handle)
                    .IsSuccess());
        CHECK(out_core == 1);
        CHECK(out_mask == (u64{1} << 1));
    }
}

TEST_CASE("DeviceMemoryManager enforces 34-bit and 36-bit bounds, allocation, and cached_pages",
          "[core][platform-sizing]") {
    const ScopedSystemTestContext context;
    const std::size_t address_bits = GENERATE(std::size_t{34}, std::size_t{36});
    CAPTURE(address_bits);

    constexpr DAddr page_size = Core::DEVICE_PAGESIZE;
    const DAddr max_area = DAddr{1} << address_bits;
    const DAddr last_valid_page = max_area - page_size;
    const DAddr first_oob_page = max_area;

    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    kernel.Initialize();
    SCOPE_EXIT {
        system.ShutdownMainProcess();
    };

    // Linear allocation fills up to and including the last valid page, then rejects overflow.
    {
        Tegra::MaxwellDeviceMemoryManager alloc_dmm{system.DeviceMemory(), address_bits};
        const size_t fill_size = static_cast<size_t>(max_area - 2 * page_size);
        const DAddr bulk = alloc_dmm.Allocate(fill_size);
        REQUIRE(bulk == page_size);

        const DAddr last_alloc = alloc_dmm.Allocate(page_size);
        CHECK(last_alloc == last_valid_page);
        CHECK(alloc_dmm.Allocate(page_size) == 0);

        alloc_dmm.Free(last_valid_page, page_size);
        CHECK(alloc_dmm.Allocate(page_size) == last_valid_page);
        CHECK(alloc_dmm.Allocate(page_size) == 0);

        alloc_dmm.Free(last_valid_page, page_size);
        alloc_dmm.Free(bulk, fill_size);
    }

    TestRasterizer rasterizer;
    Tegra::MaxwellDeviceMemoryManager dmm{system.DeviceMemory(), address_bits};
    dmm.BindInterface(&rasterizer);

    REQUIRE(dmm.DeviceAddressBits() == address_bits);
    REQUIRE(dmm.MaxDeviceArea() == max_area);

    // Bounds checks around the last valid page and first out-of-range page.
    CHECK(dmm.IsInBounds(0, page_size));
    CHECK(dmm.IsInBounds(last_valid_page, page_size));
    CHECK(dmm.IsInBounds(last_valid_page, 1));
    CHECK_FALSE(dmm.IsInBounds(first_oob_page, page_size));
    CHECK_FALSE(dmm.IsInBounds(first_oob_page, 1));
    CHECK_FALSE(dmm.IsInBounds(last_valid_page, page_size * 2));
    CHECK_FALSE(dmm.IsInBounds(last_valid_page + 1, page_size));
    CHECK_FALSE(dmm.IsInBounds(std::numeric_limits<DAddr>::max() - 0x100ULL, 0x200ULL));
    if (address_bits == 34) {
        CHECK_FALSE(dmm.IsInBounds(DAddr{1} << 34, page_size));
    } else {
        CHECK(dmm.IsInBounds(DAddr{1} << 34, page_size));
    }

    // Oversized and out-of-range allocations are rejected cleanly.
    CHECK(dmm.Allocate(0) == 0);
    CHECK(dmm.Allocate(max_area) == 0);
    CHECK_FALSE(dmm.TryAllocateFixed(0, page_size));
    CHECK_FALSE(dmm.TryAllocateFixed(first_oob_page, page_size));
    CHECK_FALSE(dmm.TryAllocateFixed(last_valid_page, page_size * 2));
    dmm.AllocateFixed(first_oob_page, page_size);
    dmm.Free(first_oob_page, page_size);

    // Fixed allocation/free on the exact last valid page.
    REQUIRE(dmm.TryAllocateFixed(last_valid_page, page_size));
    dmm.Free(last_valid_page, page_size);

    // Map guest memory onto the last valid page and verify cached_pages update/lookup and OOB rejection.
    auto* const process = Kernel::KProcess::Create(kernel);
    REQUIRE(process != nullptr);
    Kernel::KProcess::Register(kernel, process);
    SCOPE_EXIT {
        process->Close(kernel);
    };
    const auto metadata = FileSys::ProgramMetadata::GetDefault();
    REQUIRE(process->LoadFromMetadata(kernel, metadata, Kernel::PageSize, {}, 0).IsSuccess());
    process->GetMemory().SetCurrentPageTable(*process);
    REQUIRE(process->GetHandleTable().Initialize(kernel, 64).IsSuccess());
    SCOPE_EXIT {
        process->GetHandleTable().Finalize(kernel);
    };

    Kernel::KScopedResourceReservation mem_res(
        kernel, process, Kernel::Svc::LimitableResource::PhysicalMemoryMax, Kernel::PageSize);
    REQUIRE(mem_res.Succeeded());
    Kernel::KProcessAddress guest_page{};
    REQUIRE(process->GetPageTable().MapPages(&guest_page, 1, Kernel::KMemoryState::Stack,
                                             Kernel::KMemoryPermission::UserReadWrite)
                .IsSuccess());
    mem_res.Commit();
    SCOPE_EXIT {
        CHECK(process->GetPageTable().UnmapPages(guest_page, 1, Kernel::KMemoryState::Stack)
                  .IsSuccess());
    };

    const auto asid = dmm.RegisterProcess(&process->GetMemory());
    SCOPE_EXIT {
        dmm.UnregisterProcess(asid);
    };

    dmm.Map(last_valid_page, GetInteger(guest_page), page_size, asid, true);
    SCOPE_EXIT {
        dmm.Unmap(last_valid_page, page_size);
    };

    dmm.Write<u32>(last_valid_page, 0xCAFEBABEU);
    CHECK(dmm.Read<u32>(last_valid_page) == 0xCAFEBABEU);
    CHECK(dmm.GetPointer<u8>(last_valid_page) != nullptr);
    CHECK(dmm.GetSpan(last_valid_page, page_size) != nullptr);
    CHECK(dmm.GetPhysicalRawAddressFromDAddr(last_valid_page) != 0);

    CHECK(dmm.GetPageCachedCount(last_valid_page) == 0);
    dmm.UpdatePagesCachedCount(last_valid_page, page_size, 1);
    CHECK(dmm.GetPageCachedCount(last_valid_page) == 1);
    dmm.UpdatePagesCachedCount(last_valid_page, page_size, 1);
    CHECK(dmm.GetPageCachedCount(last_valid_page) == 2);
    dmm.UpdatePagesCachedCount(last_valid_page, page_size, -1);
    CHECK(dmm.GetPageCachedCount(last_valid_page) == 1);

    const std::array<std::pair<DAddr, size_t>, 2> batch_ranges{{
        {last_valid_page, page_size},
        {first_oob_page, page_size},
    }};
    dmm.UpdatePagesCachedBatch(batch_ranges, -1);
    CHECK(dmm.GetPageCachedCount(last_valid_page) == 0);

    // Out-of-range page operations must be rejected cleanly without out-of-bounds access.
    dmm.Map(first_oob_page, GetInteger(guest_page), page_size, asid, true);
    CHECK(dmm.GetPointer<u8>(first_oob_page) == nullptr);
    CHECK(dmm.GetSpan(first_oob_page, page_size) == nullptr);
    CHECK(dmm.GetPhysicalRawAddressFromDAddr(first_oob_page) == 0);
    CHECK(dmm.GetPageCachedCount(first_oob_page) == 0);

    dmm.UpdatePagesCachedCount(first_oob_page, page_size, 1);
    dmm.UpdatePagesCachedCount(last_valid_page, page_size * 2, 1);
    const std::array<std::pair<DAddr, size_t>, 1> oob_batch{{{first_oob_page, page_size}}};
    dmm.UpdatePagesCachedBatch(oob_batch, 1);
    CHECK(dmm.GetPageCachedCount(first_oob_page) == 0);
    CHECK(dmm.GetPageCachedCount(last_valid_page) == 0);
    dmm.Unmap(first_oob_page, page_size);
}

TEST_CASE("DeviceMemoryManager SetDeviceAddressBits reconfigures width cleanly",
          "[core][platform-sizing]") {
    const ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    kernel.Initialize();
    SCOPE_EXIT {
        system.ShutdownMainProcess();
    };

    Tegra::MaxwellDeviceMemoryManager dmm{system.DeviceMemory()};
    CHECK(dmm.DeviceAddressBits() == 36);
    CHECK(dmm.MaxDeviceArea() == (DAddr{1} << 36));

    // Populate multi-device-address mapping before changing address bits to verify
    // compressed_device_addr and multi_dev_address are invalidated cleanly.
    auto* const process = Kernel::KProcess::Create(kernel);
    REQUIRE(process != nullptr);
    Kernel::KProcess::Register(kernel, process);
    SCOPE_EXIT {
        process->Close(kernel);
    };
    const auto metadata = FileSys::ProgramMetadata::GetDefault();
    REQUIRE(process->LoadFromMetadata(kernel, metadata, Kernel::PageSize, {}, 0).IsSuccess());
    process->GetMemory().SetCurrentPageTable(*process);
    REQUIRE(process->GetHandleTable().Initialize(kernel, 64).IsSuccess());
    SCOPE_EXIT {
        process->GetHandleTable().Finalize(kernel);
    };
    Kernel::KScopedResourceReservation mem_res(
        kernel, process, Kernel::Svc::LimitableResource::PhysicalMemoryMax, Kernel::PageSize);
    REQUIRE(mem_res.Succeeded());
    Kernel::KProcessAddress guest_page{};
    REQUIRE(process->GetPageTable().MapPages(&guest_page, 1, Kernel::KMemoryState::Stack,
                                             Kernel::KMemoryPermission::UserReadWrite)
                .IsSuccess());
    mem_res.Commit();
    SCOPE_EXIT {
        CHECK(process->GetPageTable().UnmapPages(guest_page, 1, Kernel::KMemoryState::Stack)
                  .IsSuccess());
    };
    const auto asid = dmm.RegisterProcess(&process->GetMemory());
    SCOPE_EXIT {
        dmm.UnregisterProcess(asid);
    };

    constexpr DAddr daddr1 = 0x10000;
    constexpr DAddr daddr2 = 0x20000;
    dmm.Map(daddr1, GetInteger(guest_page), Core::DEVICE_PAGESIZE, asid, true);
    dmm.Map(daddr2, GetInteger(guest_page), Core::DEVICE_PAGESIZE, asid, true);
    const PAddr paddr = dmm.GetPhysicalRawAddressFromDAddr(daddr1);
    REQUIRE(paddr != 0);

    Common::ScratchBuffer<u32> scratch;
    std::vector<DAddr> gathered;
    dmm.ApplyOpOnPAddr(paddr, scratch, [&](DAddr addr) { gathered.push_back(addr); });
    REQUIRE(gathered.size() == 2);

    dmm.SetDeviceAddressBits(34);
    CHECK(dmm.DeviceAddressBits() == 34);
    CHECK(dmm.MaxDeviceArea() == (DAddr{1} << 34));
    CHECK(dmm.IsInBounds((DAddr{1} << 34) - Core::DEVICE_PAGESIZE, Core::DEVICE_PAGESIZE));
    CHECK_FALSE(dmm.IsInBounds(DAddr{1} << 34, Core::DEVICE_PAGESIZE));
    CHECK(dmm.GetPointer<u8>(daddr1) == nullptr);
    CHECK(dmm.GetPointer<u8>(daddr2) == nullptr);

    // After SetDeviceAddressBits clears compressed_device_addr, ApplyOpOnPAddr must not hit stale
    // MULTI_FLAG entries or emit stale device addresses.
    gathered.clear();
    dmm.ApplyOpOnPAddr(paddr, scratch, [&](DAddr addr) { gathered.push_back(addr); });
    CHECK(std::find(gathered.begin(), gathered.end(), daddr1) == gathered.end());
    CHECK(std::find(gathered.begin(), gathered.end(), daddr2) == gathered.end());

    // Out-of-range physical address must be ignored cleanly.
    gathered.clear();
    dmm.ApplyOpOnPAddr(PAddr{1} << 40, scratch, [&](DAddr addr) { gathered.push_back(addr); });
    CHECK(gathered.empty());
}

TEST_CASE("KernelCore and CpuManager default to 4 active cores and guard post-init updates",
          "[core][platform-sizing]") {
    const ScopedSystemTestContext context;
    Core::System system;
    auto& kernel = system.Kernel();
    auto& cpu_manager = system.GetCpuManager();

    CHECK(kernel.ActiveCoreCount() == 4);
    CHECK(cpu_manager.ActiveCoreCount() == 4);
    CHECK_FALSE(kernel.IsInitialized());

    system.Initialize();
    CHECK(kernel.ActiveCoreCount() == 4);
    CHECK(cpu_manager.ActiveCoreCount() == 4);
    CHECK_FALSE(kernel.IsInitialized());

    kernel.SetMulticore(true);
    cpu_manager.SetMulticore(true);
    cpu_manager.SetAsyncGpu(true);

    kernel.Initialize();
    CHECK(kernel.IsInitialized());
    system.CoreTiming().SyncPause(false);
    cpu_manager.Initialize();
    cpu_manager.OnGpuReady();

    // Post-initialization mutations must be ignored so active core count cannot diverge from
    // initialized schedulers/cores/threads.
    kernel.SetActiveCoreCount(6);
    CHECK(kernel.ActiveCoreCount() == 4);
    cpu_manager.SetActiveCoreCount(6);
    CHECK(cpu_manager.ActiveCoreCount() == 4);

    system.ShutdownMainProcess();
    CHECK_FALSE(kernel.IsInitialized());

    kernel.SetActiveCoreCount(6);
    cpu_manager.SetActiveCoreCount(6);
    CHECK(kernel.ActiveCoreCount() == 6);
    CHECK(cpu_manager.ActiveCoreCount() == 6);
}

TEST_CASE("FlatAllocator handles zero size, fixed overlaps, partial frees, and wrap-around",
          "[core][platform-sizing]") {
    Common::FlatAllocator<u32, 0, 32> alloc(0x1000, 0x6000);

    CHECK(alloc.Allocate(0) == 0);
    alloc.AllocateFixed(0x500, 0x500);
    alloc.AllocateFixed(0x5000, 0x2000);
    alloc.AllocateFixed(0xFFFFF000U, 0x2000);
    alloc.AllocateFixed(0x1000, 0);
    alloc.Free(0x1000, 0);

    // Fixed mapping right at current_linear_alloc_end must be skipped cleanly.
    alloc.AllocateFixed(0x1000, 0x1000);
    CHECK(alloc.Allocate(0x1000) == 0x2000);

    // Partial tail free must not assert, and subsequent wrap-around must find the freed tail.
    CHECK(alloc.Allocate(0x2000) == 0x3000);
    alloc.Free(0x4000, 0x1000);
    CHECK(alloc.Allocate(0x1000) == 0x5000);
    CHECK(alloc.Allocate(0x1000) == 0x4000);
    CHECK(alloc.Allocate(0x1000) == 0);

    // Freeing an adjacent block must erase it instead of leaking the slot.
    alloc.Free(0x2000, 0x1000);
    CHECK(alloc.Allocate(0x1000) == 0x2000);

    // Freeing everything (leaving a single block) must still allow wrap-around allocation.
    alloc.Free(0x1000, 0x5000);
    CHECK(alloc.Allocate(0x6000) == 0);
    CHECK(alloc.Allocate(0x5000) == 0x1000);
    CHECK(alloc.Allocate(0x1000) == 0);
    alloc.Free(0x1000, 0x5000);

    // Zero base start edge case must not corrupt block invariants or decrement past begin().
    Common::FlatAllocator<u32, 0, 32> zero_base(0, 0x4000);
    zero_base.AllocateFixed(0, 0x1000);
    CHECK(zero_base.Allocate(0x1000) == 0x1000);
    zero_base.Free(0, 0x1000);
    zero_base.AllocateFixed(0, 0x1000);
    CHECK(zero_base.Allocate(0x2000) == 0x2000);
    zero_base.Free(0, 0x4000);
}

TEST_CASE("SparseLargeVector enforces exclusive bounds and clears on ResizeAndClear",
          "[core][platform-sizing]") {
    constexpr std::size_t count = 32;
    Common::SparseLargeVector<u32> vec(count);
    REQUIRE(vec.size() == count);

    vec.Set(0, 0x11111111U);
    vec.Set(count - 1, 0x22222222U);
    vec.Set(count, 0xDEADBEEFU); // exact end index is out of bounds
    CHECK(vec[0] == 0x11111111U);
    CHECK(vec[count - 1] == 0x22222222U);
    CHECK(vec[count] == 0U);

    // Resizing to the same count must still clear all entries.
    vec.ResizeAndClear(count);
    CHECK(vec.size() == count);
    CHECK(vec[0] == 0U);
    CHECK(vec[count - 1] == 0U);

    vec.GetAndFault(count - 1) = 0x33333333U;
    CHECK(vec[count - 1] == 0x33333333U);

    vec.ResizeAndClear(count * 2);
    CHECK(vec.size() == count * 2);
    CHECK(vec[count - 1] == 0U);
    vec.Set(count * 2 - 1, 0x44444444U);
    CHECK(vec[count * 2 - 1] == 0x44444444U);

    vec.ResizeAndClear(0);
    CHECK(vec.size() == 0);
    CHECK(vec[0] == 0U);

    vec.ResizeAndClear(16);
    CHECK(vec.size() == 16);
    vec.Set(15, 0x55555555U);
    CHECK(vec[15] == 0x55555555U);
}

TEST_CASE("DeviceMemoryManager handles block edge cases, page crossings, unaligned maps, and dual ASIDs",
          "[core][platform-sizing]") {
    const ScopedSystemTestContext context;
    constexpr DAddr page_size = Core::DEVICE_PAGESIZE;

    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    kernel.Initialize();
    SCOPE_EXIT {
        system.ShutdownMainProcess();
    };

    auto make_process = [&]() -> Kernel::KProcess* {
        auto* const proc = Kernel::KProcess::Create(kernel);
        REQUIRE(proc != nullptr);
        Kernel::KProcess::Register(kernel, proc);
        const auto metadata = FileSys::ProgramMetadata::GetDefault();
        REQUIRE(proc->LoadFromMetadata(kernel, metadata, Kernel::PageSize, {}, 0).IsSuccess());
        proc->GetMemory().SetCurrentPageTable(*proc);
        REQUIRE(proc->GetHandleTable().Initialize(kernel, 64).IsSuccess());
        return proc;
    };

    Kernel::KProcess* const proc_a = make_process();
    SCOPE_EXIT {
        proc_a->GetHandleTable().Finalize(kernel);
        proc_a->Close(kernel);
    };
    Kernel::KProcess* const proc_b = make_process();
    SCOPE_EXIT {
        proc_b->GetHandleTable().Finalize(kernel);
        proc_b->Close(kernel);
    };

    auto map_guest_pages = [&](Kernel::KProcess* proc, size_t num_pages) -> Kernel::KProcessAddress {
        Kernel::KScopedResourceReservation mem_res(
            kernel, proc, Kernel::Svc::LimitableResource::PhysicalMemoryMax,
            num_pages * Kernel::PageSize);
        REQUIRE(mem_res.Succeeded());
        Kernel::KProcessAddress addr{};
        REQUIRE(proc->GetPageTable()
                    .MapPages(&addr, num_pages, Kernel::KMemoryState::Stack,
                              Kernel::KMemoryPermission::UserReadWrite)
                    .IsSuccess());
        mem_res.Commit();
        return addr;
    };

    const Kernel::KProcessAddress guest_a = map_guest_pages(proc_a, 3);
    SCOPE_EXIT {
        CHECK(proc_a->GetPageTable().UnmapPages(guest_a, 3, Kernel::KMemoryState::Stack).IsSuccess());
    };
    const Kernel::KProcessAddress guest_b = map_guest_pages(proc_b, 2);
    SCOPE_EXIT {
        CHECK(proc_b->GetPageTable().UnmapPages(guest_b, 2, Kernel::KMemoryState::Stack).IsSuccess());
    };

    // Null device_inter and zero-size block calls must not crash.
    Tegra::MaxwellDeviceMemoryManager dmm{system.DeviceMemory(), 34};
    const auto asid_a = dmm.RegisterProcess(&proc_a->GetMemory());
    const auto asid_b = dmm.RegisterProcess(&proc_b->GetMemory());
    SCOPE_EXIT {
        dmm.UnregisterProcess(asid_b);
        dmm.UnregisterProcess(asid_a);
    };

    // Map two non-contiguous physical pages (reverse guest order) and verify page-crossing Read/Write.
    constexpr DAddr cross_base = 0x10000;
    dmm.Map(cross_base, GetInteger(guest_a) + page_size, page_size, asid_a, false);
    dmm.Map(cross_base + page_size, GetInteger(guest_a), page_size, asid_a, false);
    SCOPE_EXIT {
        dmm.Unmap(cross_base, page_size * 2);
    };

    std::array<u8, 4> pattern{0x11, 0x22, 0x33, 0x44};
    dmm.WriteBlock(cross_base, pattern.data(), pattern.size());
    std::array<u8, 4> read_back{};
    dmm.ReadBlock(cross_base, read_back.data(), read_back.size());
    CHECK(read_back == pattern);

    dmm.WriteBlock(cross_base, nullptr, 0);
    dmm.ReadBlock(cross_base, nullptr, 0);
    dmm.WriteBlockUnsafe(cross_base, nullptr, 0);
    dmm.ReadBlockUnsafe(cross_base, nullptr, 0);

    dmm.Write<u32>(cross_base + page_size - 2, 0xAABBCCDDU);
    CHECK(dmm.Read<u32>(cross_base + page_size - 2) == 0xAABBCCDDU);
    CHECK(*dmm.GetPointer<u8>(cross_base + page_size - 2) == 0xDD);
    CHECK(*dmm.GetPointer<u8>(cross_base + page_size - 1) == 0xCC);
    CHECK(*dmm.GetPointer<u8>(cross_base + page_size) == 0xBB);
    CHECK(*dmm.GetPointer<u8>(cross_base + page_size + 1) == 0xAA);

    // Out-of-bounds Read<T>/Write<T> straddling MaxDeviceArea() must be rejected completely.
    const DAddr last_page = dmm.MaxDeviceArea() - page_size;
    dmm.Map(last_page, GetInteger(guest_a) + 2 * page_size, page_size, asid_a, true);
    SCOPE_EXIT {
        dmm.Unmap(last_page, page_size);
    };
    dmm.Write<u32>(last_page + page_size - 4, 0xDEADBEEFU);
    dmm.Write<u32>(dmm.MaxDeviceArea() - 2, 0x12345678U);
    CHECK(dmm.Read<u32>(dmm.MaxDeviceArea() - 2) == 0U);
    CHECK(dmm.Read<u32>(last_page + page_size - 4) == 0xDEADBEEFU);

    // Unaligned Map/Unmap spanning two pages must cover both pages.
    constexpr DAddr unaligned_addr = 0x20800;
    dmm.Map(unaligned_addr, GetInteger(guest_a), page_size, asid_a, true);
    CHECK(dmm.GetPointer<u8>(0x20000) != nullptr);
    CHECK(dmm.GetPointer<u8>(0x21000) != nullptr);
    dmm.Unmap(unaligned_addr, page_size);
    CHECK(dmm.GetPointer<u8>(0x20000) == nullptr);
    CHECK(dmm.GetPointer<u8>(0x21000) == nullptr);

    // Dual-ASID UpdatePagesCachedCount must track asid transitions accurately even when vpages
    // appear contiguous across processes.
    constexpr DAddr dual_base = 0x30000;
    dmm.Map(dual_base + 0 * page_size, GetInteger(guest_a) + 0 * page_size, page_size, asid_a);
    dmm.Map(dual_base + 1 * page_size, GetInteger(guest_b) + 1 * page_size, page_size, asid_b);
    dmm.Map(dual_base + 2 * page_size, GetInteger(guest_a) + 2 * page_size, page_size, asid_a);
    SCOPE_EXIT {
        dmm.Unmap(dual_base, page_size * 3);
    };

    dmm.UpdatePagesCachedCount(dual_base, page_size * 3, 1);
    CHECK(proc_a->GetPageTable().GetImpl().entries[GetInteger(guest_a) >> 12].Type() ==
          Common::PageType::RasterizerCachedMemory);
    CHECK(proc_b->GetPageTable().GetImpl().entries[(GetInteger(guest_b) + page_size) >> 12].Type() ==
          Common::PageType::RasterizerCachedMemory);
    CHECK(proc_a->GetPageTable()
              .GetImpl()
              .entries[(GetInteger(guest_a) + 2 * page_size) >> 12]
              .Type() == Common::PageType::RasterizerCachedMemory);

    dmm.UpdatePagesCachedCount(dual_base, page_size * 3, -1);
    CHECK(proc_a->GetPageTable().GetImpl().entries[GetInteger(guest_a) >> 12].Type() ==
          Common::PageType::Memory);
    CHECK(proc_b->GetPageTable().GetImpl().entries[(GetInteger(guest_b) + page_size) >> 12].Type() ==
          Common::PageType::Memory);
    CHECK(proc_a->GetPageTable()
              .GetImpl()
              .entries[(GetInteger(guest_a) + 2 * page_size) >> 12]
              .Type() == Common::PageType::Memory);

    // ArmReadTraps with unregistered/null process slots must not crash.
    dmm.UnregisterProcess(asid_b);
    dmm.ArmReadTraps(dual_base, page_size * 3);
}

TEST_CASE("SVC thread core mask validation checks pre-fold caps, IdealCoreUseProcessValue, and zero collapse",
          "[core][platform-sizing]") {
    const ScopedSystemTestContext context;
    const bool is_switch2 = GENERATE(false, true);
    const u32 active_cores = is_switch2 ? 6U : 4U;
    CAPTURE(is_switch2, active_cores);

    Core::System system;
    system.Initialize();
    system.SetPlatformProfile(Core::MakePlatformProfile(
        is_switch2 ? Core::PlatformResolution::ConfirmedSwitch2
                   : Core::PlatformResolution::ConfirmedSwitch1));
    auto& kernel = system.Kernel();
    kernel.SetActiveCoreCount(active_cores);
    kernel.Initialize();
    SCOPE_EXIT {
        system.ShutdownMainProcess();
    };

    auto* const process = Kernel::KProcess::Create(kernel);
    REQUIRE(process != nullptr);
    Kernel::KProcess::Register(kernel, process);
    SCOPE_EXIT {
        process->Close(kernel);
    };

    // Allow cores 0..5 only on Switch 2 (or 0..15 via capability to also test pre-fold collapse)
    // in separate sections below.
    FileSys::ProgramMetadata metadata;
    metadata.LoadManual(true, FileSys::ProgramAddressSpaceType::Is39Bit, 0x2c, 0, 0x100000,
                        0x0100123400001000ULL, 0xFFFFFFFFFFFFFFFFULL, 0,
                        {0x050043F7U, (64U << 16) | 0x7FFFU});
    REQUIRE(process->LoadFromMetadata(kernel, metadata, Kernel::PageSize, {}, 0).IsSuccess());
    process->GetMemory().SetCurrentPageTable(*process);
    REQUIRE(process->GetHandleTable().Initialize(kernel, 64).IsSuccess());
    SCOPE_EXIT {
        process->GetHandleTable().Finalize(kernel);
    };

    Kernel::KScopedResourceReservation owner_res(
        kernel, process, Kernel::Svc::LimitableResource::ThreadCountMax);
    REQUIRE(owner_res.Succeeded());
    Kernel::KThread* const owner = Kernel::KThread::Create(kernel);
    REQUIRE(owner != nullptr);
    REQUIRE(Kernel::KThread::InitializeUserThread(system, owner, {}, 0, 0x100000, 0x2c, 0, process)
                .IsSuccess());
    Kernel::KThread::Register(kernel, owner);
    owner_res.Commit();
    SCOPE_EXIT {
        kernel.GlobalSchedulerContext().RemoveThread(owner);
        owner->Close(kernel);
    };

    auto* const original_thread = Kernel::GetCurrentThreadPointer(kernel);
    Kernel::SetCurrentThread(kernel, owner);
    owner->EnableDispatch(kernel);
    SCOPE_EXIT {
        owner->DisableDispatch(kernel);
        Kernel::SetCurrentThread(kernel, original_thread);
    };

    // Core 6 is not in the 0..5 process core mask and must be rejected BEFORE Switch 2 folding.
    Kernel::Handle bad_handle = Kernel::Svc::InvalidHandle;
    CHECK(Kernel::Svc::CreateThread64(system, &bad_handle, 0x100000, 0, 0x200000, 0x2c, 6) ==
          Kernel::ResultInvalidCoreId);
    CHECK(bad_handle == Kernel::Svc::InvalidHandle);

    // Direct KThread::SetCoreMask enforces active core mask bounds.
    CHECK(owner->SetCoreMask(kernel, 0, u64{1} << active_cores) == Kernel::ResultInvalidCoreId);

    // Test IdealCoreUseProcessValue and high-bit collapse guards with wide capability bits (0..15).
    auto* const wide_process = Kernel::KProcess::Create(kernel);
    REQUIRE(wide_process != nullptr);
    Kernel::KProcess::Register(kernel, wide_process);
    SCOPE_EXIT {
        wide_process->Close(kernel);
    };
    FileSys::ProgramMetadata wide_metadata;
    wide_metadata.LoadManual(true, FileSys::ProgramAddressSpaceType::Is39Bit, 0x2c, 0, 0x100000,
                             0x0100123400002000ULL, 0xFFFFFFFFFFFFFFFFULL, 0,
                             {0x0F0043F7U, (64U << 16) | 0x7FFFU});
    REQUIRE(
        wide_process->LoadFromMetadata(kernel, wide_metadata, Kernel::PageSize, {}, 0).IsSuccess());
    wide_process->GetMemory().SetCurrentPageTable(*wide_process);
    REQUIRE(wide_process->GetHandleTable().Initialize(kernel, 64).IsSuccess());
    SCOPE_EXIT {
        wide_process->GetHandleTable().Finalize(kernel);
    };

    Kernel::KScopedResourceReservation wide_owner_res(
        kernel, wide_process, Kernel::Svc::LimitableResource::ThreadCountMax);
    REQUIRE(wide_owner_res.Succeeded());
    Kernel::KThread* const wide_owner = Kernel::KThread::Create(kernel);
    REQUIRE(wide_owner != nullptr);
    REQUIRE(Kernel::KThread::InitializeUserThread(system, wide_owner, {}, 0, 0x100000, 0x2c, 0,
                                                wide_process)
                .IsSuccess());
    Kernel::KThread::Register(kernel, wide_owner);
    wide_owner_res.Commit();
    SCOPE_EXIT {
        kernel.GlobalSchedulerContext().RemoveThread(wide_owner);
        wide_owner->Close(kernel);
    };

    Kernel::SetCurrentThread(kernel, wide_owner);
    wide_owner->EnableDispatch(kernel);
    SCOPE_EXIT {
        wide_owner->DisableDispatch(kernel);
        Kernel::SetCurrentThread(kernel, owner);
    };

    Kernel::Handle wide_thread = Kernel::Svc::InvalidHandle;
    REQUIRE(Kernel::Svc::CreateThread64(system, &wide_thread, 0x100000, 0, 0x200000, 0x2c, 0)
                .IsSuccess());
    SCOPE_EXIT {
        if (auto obj = wide_process->GetHandleTable().GetObject<Kernel::KThread>(kernel, wide_thread);
            obj.IsNotNull()) {
            kernel.GlobalSchedulerContext().RemoveThread(obj.GetPointerUnsafe());
        }
        CHECK(Kernel::Svc::CloseHandle64(system, wide_thread).IsSuccess());
    };

    // High pre-fold core bits (8, 12) must fail instead of collapsing to 0 or sneaking past folding.
    CHECK(Kernel::Svc::SetThreadCoreMask64(system, wide_thread, Kernel::Svc::IdealCoreDontCare,
                                           u64{1} << 8) == Kernel::ResultInvalidCoreId);
    CHECK(Kernel::Svc::SetThreadCoreMask64(system, wide_thread, Kernel::Svc::IdealCoreDontCare,
                                           u64{1} << 12) == Kernel::ResultInvalidCoreId);

    // Pre-fold core_id must belong to pre-fold affinity_mask.
    CHECK(Kernel::Svc::SetThreadCoreMask64(system, wide_thread, 6, u64{1} << 0) ==
          (is_switch2 ? Kernel::ResultInvalidCombination : Kernel::ResultInvalidCoreId));

    if (is_switch2) {
        wide_process->SetIdealCoreId(6);
        REQUIRE(Kernel::Svc::SetThreadCoreMask64(system, wide_thread,
                                                 Kernel::Svc::IdealCoreUseProcessValue, 0)
                    .IsSuccess());
        s32 out_core = -1;
        u64 out_mask = 0;
        REQUIRE(Kernel::Svc::GetThreadCoreMask64(system, &out_core, &out_mask, wide_thread)
                    .IsSuccess());
        CHECK(out_core == 0);
        CHECK(out_mask == (u64{1} << 0));
    } else {
        wide_process->SetIdealCoreId(4);
        CHECK(Kernel::Svc::SetThreadCoreMask64(system, wide_thread,
                                               Kernel::Svc::IdealCoreUseProcessValue, 0) ==
              Kernel::ResultInvalidCoreId);
    }
}
