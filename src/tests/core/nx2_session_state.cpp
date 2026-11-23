// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/file_sys/program_metadata.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/hle/service/nvdrv/nvdata.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/os/process.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "core/nx2_session_state.h"
#include "core/platform_profile.h"
#include "tests/core/system_test_context.h"
#include "video_core/gpu.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/host1x/host1x.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/query_cache/query_cache.h"
#include "video_core/rasterizer_interface.h"

namespace {

class TestQueryRasterizer final : public VideoCore::RasterizerInterface {
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
    void SignalFence(std::function<void()>&& operation, bool) override {
        ++fences;
        if (defer) {
            pending.push_back(std::move(operation));
        } else {
            operation();
        }
    }
    void SyncOperation(std::function<void()>&& operation) override {
        pending.push_back(std::move(operation));
    }
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

    bool defer{};
    u32 fences{};
    std::vector<std::function<void()>> pending;
};

class TestPayloadStreamer final : public VideoCommon::StreamerInterface {
public:
    TestPayloadStreamer() : StreamerInterface{0} {
        amend_value = 7;
    }

    size_t WriteCounter(VAddr address, bool timestamp, u32 value, std::optional<u32>) override {
        query = VideoCommon::GuestQuery{timestamp, address, value};
        return 0;
    }
    VideoCommon::QueryBase* GetQuery(size_t) override {
        return &query;
    }
    void Free(size_t) override {
        ++freed;
    }

    VideoCommon::GuestQuery query{false, 0, 0};
    u32 freed{};
};

struct TestQueryRuntime {
    VideoCommon::StreamerInterface* GetStreamerInterface(VideoCommon::QueryType type) {
        return type == VideoCommon::QueryType::Payload ? &streamer : nullptr;
    }
    void Bind3DEngine(Tegra::Engines::Maxwell3D*) {}

    template <typename T>
    void SyncValues(std::span<T>) {}

    TestPayloadStreamer streamer;
};

struct TestQueryTraits {
    using RuntimeType = TestQueryRuntime;
};

class TestQueryCache final : public VideoCommon::QueryCacheBase<TestQueryTraits> {
public:
    using QueryCacheBase::QueryCacheBase;
    using QueryCacheBase::UnregisterPending;

    void UseMemory(Tegra::MemoryManager& memory) {
        gpu_memory = &memory;
    }
};

class TestSessionLoader final : public Loader::AppLoader {
public:
    TestSessionLoader() : AppLoader(nullptr) {}

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

struct QueryTestHarness {
    static constexpr DAddr CpuAddr = 0x1000;
    static constexpr GPUVAddr GpuAddr = 0x10000;

    explicit QueryTestHarness(Core::System& system_)
        : system{system_}, device_memory{dram}, memory{system, device_memory, 32, 1ULL << 31},
          gpu{system, false, false}, cache{gpu, rasterizer, device_memory, runtime} {
        page = dram.buffer.BackingBasePointer() + 0x10000;
        {
            Kernel::KProcess process{system.Kernel()};
            SCOPE_EXIT {
                system.Kernel().UnregisterKernelObject(&process);
            };
            auto& table = process.GetPageTable().GetImpl();
            table.Resize(20, 12);
            table.entries.GetAndFault(4).Store(false, Common::PageType::Memory, 1,
                                             reinterpret_cast<uintptr_t>(page) - 0x4000);
            process.GetMemory().SetCurrentPageTable(process);
            const auto asid = device_memory.RegisterProcess(&process.GetMemory());
            device_memory.Map(CpuAddr, 0x4000, 0x1000, asid);
            device_memory.UnregisterProcess(asid);
        }
        device_memory.BindInterface(&rasterizer);
        memory.BindRasterizer(&rasterizer);
        memory.Map(GpuAddr, CpuAddr, 0x1000, Tegra::PTEKind::PITCH, false);
        cache.UseMemory(memory);
    }

    Core::System& system;
    Core::DeviceMemory dram;
    u8* page{};
    Tegra::MaxwellDeviceMemoryManager device_memory;
    TestQueryRasterizer rasterizer;
    Tegra::MemoryManager memory;
    Tegra::GPU gpu;
    TestQueryRuntime runtime;
    TestQueryCache cache;
};

} // namespace

TEST_CASE("Switch 1 query completion safely no-ops absent Nx2SessionState",
          "[core][nx2-session]") {
    using VideoCommon::QueryPropertiesFlags;
    using VideoCommon::QueryType;

    ScopedSystemTestContext context;
    Core::System system;
    system.SetPlatformProfile(Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch1));
    system.Initialize();
    REQUIRE(system.Nx2Session() == nullptr);

    QueryTestHarness harness{system};
    const u16 slot = static_cast<u16>(Tegra::Nx2AsyncReports::Slot(QueryTestHarness::CpuAddr));

    const auto old_behavior = Settings::values.gpu_fence_behavior.GetValue();
    const auto old_accuracy = Settings::values.current_gpu_accuracy;
    SCOPE_EXIT {
        Settings::values.gpu_fence_behavior.SetValue(old_behavior);
        Settings::values.current_gpu_accuracy = old_accuracy;
        Settings::SetCurrentProgramIsSwitch2(false);
    };

    int global_waiter_owner{};
    constexpr u32 ImmediatePayload = 0x42U;
    constexpr u32 DeferredPayload = 0x77U;
    Tegra::Nx2SemaphoreWaiters::Get().Add(&global_waiter_owner, QueryTestHarness::CpuAddr,
                                          ImmediatePayload,
                                          Tegra::Nx2SemaphoreWaiters::AcquireEqual);
    SCOPE_EXIT {
        Tegra::Nx2SemaphoreWaiters::Get().Remove(&global_waiter_owner);
    };

    const u32 global_issued_before =
        Tegra::g_nx2_async_reports_issued[slot].load(std::memory_order_acquire);
    const u32 global_completed_before =
        Tegra::g_nx2_async_reports_completed[slot].load(std::memory_order_acquire);

    // Immediate non-fence Payload path skips Nx2SemaphoreWaiters without touching global state.
    Settings::values.current_gpu_accuracy = Settings::GpuAccuracy::Low;
    Settings::values.gpu_fence_behavior.SetValue(Settings::GpuFenceBehavior::Immediate);
    std::memset(harness.page, 0x5a, 16);
    harness.cache.CounterReport(QueryTestHarness::GpuAddr, QueryType::Payload,
                                QueryPropertiesFlags{}, ImmediatePayload, 0);
    CHECK(harness.device_memory.Read<u32>(QueryTestHarness::CpuAddr) == ImmediatePayload);
    CHECK(harness.rasterizer.pending.empty());
    CHECK_FALSE(Tegra::Nx2SemaphoreWaiters::Get().Latched(&global_waiter_owner));
    CHECK(Tegra::g_nx2_async_reports_issued[slot].load(std::memory_order_acquire) ==
          global_issued_before);
    CHECK(Tegra::g_nx2_async_reports_completed[slot].load(std::memory_order_acquire) ==
          global_completed_before);

    // Deferred Settle path executes cleanly while leaving global counters and waiters untouched.
    Tegra::Nx2SemaphoreWaiters::Get().Add(&global_waiter_owner, QueryTestHarness::CpuAddr,
                                          DeferredPayload + 7U,
                                          Tegra::Nx2SemaphoreWaiters::AcquireEqual);
    Settings::values.current_gpu_accuracy = Settings::GpuAccuracy::High;
    Settings::values.gpu_fence_behavior.SetValue(Settings::GpuFenceBehavior::Accurate);
    harness.rasterizer.defer = true;
    std::memset(harness.page, 0x5a, 16);
    harness.cache.CounterReport(QueryTestHarness::GpuAddr, QueryType::Payload,
                                QueryPropertiesFlags::IsAFence, DeferredPayload, 0);
    REQUIRE(harness.rasterizer.pending.size() == 1);
    CHECK(Tegra::g_nx2_async_reports_issued[slot].load(std::memory_order_acquire) ==
          global_issued_before);

    harness.rasterizer.pending.front()();
    harness.rasterizer.pending.clear();
    harness.cache.UnregisterPending();

    CHECK(harness.device_memory.Read<u32>(QueryTestHarness::CpuAddr) == DeferredPayload + 7U);
    CHECK_FALSE(Tegra::Nx2SemaphoreWaiters::Get().Latched(&global_waiter_owner));
    CHECK(Tegra::g_nx2_async_reports_issued[slot].load(std::memory_order_acquire) ==
          global_issued_before);
    CHECK(Tegra::g_nx2_async_reports_completed[slot].load(std::memory_order_acquire) ==
          global_completed_before);

    // Immediate and deferred timestamped paths also stay no-ops on Switch 1 globals.
    Settings::values.current_gpu_accuracy = Settings::GpuAccuracy::Low;
    Settings::values.gpu_fence_behavior.SetValue(Settings::GpuFenceBehavior::Immediate);
    harness.rasterizer.defer = false;
    std::memset(harness.page, 0x5a, 16);
    harness.cache.CounterReport(QueryTestHarness::GpuAddr, QueryType::Payload,
                                QueryPropertiesFlags::HasTimeout, ImmediatePayload, 0);
    CHECK(harness.device_memory.Read<u64>(QueryTestHarness::CpuAddr) == ImmediatePayload);
    CHECK_FALSE(Tegra::Nx2SemaphoreWaiters::Get().Latched(&global_waiter_owner));
    CHECK(Tegra::g_nx2_async_reports_issued[slot].load(std::memory_order_acquire) ==
          global_issued_before);
    CHECK(Tegra::g_nx2_async_reports_completed[slot].load(std::memory_order_acquire) ==
          global_completed_before);

    Settings::values.current_gpu_accuracy = Settings::GpuAccuracy::High;
    Settings::values.gpu_fence_behavior.SetValue(Settings::GpuFenceBehavior::Accurate);
    harness.rasterizer.defer = true;
    std::memset(harness.page, 0x5a, 16);
    harness.cache.CounterReport(QueryTestHarness::GpuAddr, QueryType::Payload,
                                QueryPropertiesFlags::IsAFence | QueryPropertiesFlags::HasTimeout,
                                DeferredPayload, 0);
    REQUIRE(harness.rasterizer.pending.size() == 1);
    harness.rasterizer.pending.front()();
    harness.rasterizer.pending.clear();
    harness.cache.UnregisterPending();
    CHECK(harness.device_memory.Read<u64>(QueryTestHarness::CpuAddr) == DeferredPayload + 7U);
    CHECK_FALSE(Tegra::Nx2SemaphoreWaiters::Get().Latched(&global_waiter_owner));
    CHECK(Tegra::g_nx2_async_reports_issued[slot].load(std::memory_order_acquire) ==
          global_issued_before);
    CHECK(Tegra::g_nx2_async_reports_completed[slot].load(std::memory_order_acquire) ==
          global_completed_before);
}

TEST_CASE("Switch 1 nvdrv CloseSession skips global Nx2GpuMemoryRegistry",
          "[core][nx2-session]") {
    using Service::Nvidia::Devices::Nx2GpuMemoryRegistry;
    using Service::Nvidia::NvCore::Container;

    ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    system.Kernel().Initialize();

    std::optional<Service::Process> process;
    std::unique_ptr<Tegra::Host1x::Host1x> host1x;
    std::optional<Container> container;
    SCOPE_EXIT {
        container.reset();
        host1x.reset();
        process.reset();
        system.ShutdownMainProcess();
        Settings::SetCurrentProgramIsSwitch2(false);
    };

    process.emplace(system);
    Loader::ResultStatus status = Loader::ResultStatus::Success;
    TestSessionLoader loader;
    REQUIRE(process->Initialize(loader, status));
    REQUIRE(process->IsInitialized());

    host1x = std::make_unique<Tegra::Host1x::Host1x>(system);
    container.emplace(*host1x);

    REQUIRE(system.Nx2Session() == nullptr);
    const auto sw1_sid = container->OpenSession(process->GetHandle());
    size_t sw1_asid{};
    REQUIRE(container->WithSession(
        sw1_sid, [&](const Service::Nvidia::NvCore::Session& s) { sw1_asid = s.asid.id; }));

    auto& global_registry = Nx2GpuMemoryRegistry::Get();
    SCOPE_EXIT {
        global_registry.ForgetDeviceRanges(sw1_asid);
    };
    u32 global_allocs = 0;
    const u64 global_addr =
        global_registry.AcquireDeviceRange(sw1_asid, 0x10000, 0x1000, [&] {
            ++global_allocs;
            return 0x50000000ULL;
        });
    REQUIRE(global_addr == 0x50000000ULL);
    REQUIRE(global_allocs == 1);

    container->CloseSession(sw1_sid);
    CHECK(container->GetSession(sw1_sid) == nullptr);

    // Global registry must still hold the range because Switch 1 CloseSession is a no-op on NX2.
    const u64 preserved =
        global_registry.AcquireDeviceRange(sw1_asid, 0x10000, 0x1000, [&] {
            ++global_allocs;
            return 0x51000000ULL;
        });
    CHECK(preserved == 0x50000000ULL);
    CHECK(global_allocs == 1);

    // Switch 2 CloseSession forgets device ranges in the active Nx2SessionState only.
    container.reset();
    host1x.reset();
    process.reset();
    system.ShutdownMainProcess();

    system.SetPlatformProfile(Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch2));
    system.Initialize();
    system.Kernel().Initialize();

    process.emplace(system);
    REQUIRE(process->Initialize(loader, status));
    REQUIRE(process->IsInitialized());
    host1x = std::make_unique<Tegra::Host1x::Host1x>(system);
    container.emplace(*host1x);
    auto sw2_session = system.Nx2Session();
    REQUIRE(sw2_session != nullptr);

    const auto sw2_sid = container->OpenSession(process->GetHandle());
    size_t sw2_asid{};
    REQUIRE(container->WithSession(
        sw2_sid, [&](const Service::Nvidia::NvCore::Session& s) { sw2_asid = s.asid.id; }));

    u32 session_allocs = 0;
    const u64 session_addr =
        sw2_session->GpuMemoryRegistry().AcquireDeviceRange(sw2_asid, 0x20000, 0x1000, [&] {
            ++session_allocs;
            return 0x60000000ULL;
        });
    REQUIRE(session_addr == 0x60000000ULL);
    REQUIRE(session_allocs == 1);

    container->CloseSession(sw2_sid);
    CHECK(container->GetSession(sw2_sid) == nullptr);

    const u64 reallocated =
        sw2_session->GpuMemoryRegistry().AcquireDeviceRange(sw2_asid, 0x20000, 0x1000, [&] {
            ++session_allocs;
            return 0x70000000ULL;
        });
    CHECK(reallocated == 0x70000000ULL);
    CHECK(session_allocs == 2);
}

TEST_CASE("Deferred query callbacks safely finish before or retire across session teardown",
          "[core][nx2-session]") {
    using VideoCommon::QueryPropertiesFlags;
    using VideoCommon::QueryType;

    ScopedSystemTestContext context;
    Core::System system;
    system.SetPlatformProfile(Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch2));
    system.Initialize();
    system.Kernel().Initialize();

    const auto old_behavior = Settings::values.gpu_fence_behavior.GetValue();
    const auto old_accuracy = Settings::values.current_gpu_accuracy;
    SCOPE_EXIT {
        Settings::values.gpu_fence_behavior.SetValue(old_behavior);
        Settings::values.current_gpu_accuracy = old_accuracy;
        Settings::SetCurrentProgramIsSwitch2(false);
    };
    Settings::values.current_gpu_accuracy = Settings::GpuAccuracy::High;
    Settings::values.gpu_fence_behavior.SetValue(Settings::GpuFenceBehavior::Accurate);

    QueryTestHarness harness{system};
    harness.rasterizer.defer = true;
    const u16 slot = static_cast<u16>(Tegra::Nx2AsyncReports::Slot(QueryTestHarness::CpuAddr));

    auto session1 = system.Nx2Session();
    REQUIRE(session1 != nullptr);
    const Core::Nx2SessionState::Handle s1_handle = session1->GetHandle();
    REQUIRE(s1_handle != 0);

    int waiter_owner{};
    constexpr u32 Payload1 = 0x123U;
    session1->SemaphoreWaiters().Add(&waiter_owner, QueryTestHarness::CpuAddr, Payload1 + 7U,
                                     Tegra::Nx2SemaphoreWaiters::AcquireEqual);

    // Callback finishing while session 1 is active settles counters and latches its waiter.
    std::memset(harness.page, 0x5a, 16);
    harness.cache.CounterReport(QueryTestHarness::GpuAddr, QueryType::Payload,
                                QueryPropertiesFlags::IsAFence, Payload1, 0);
    CHECK(session1->AsyncReports().Issued(slot) == 1);
    CHECK(session1->AsyncReports().Completed(slot) == 0);
    CHECK_FALSE(session1->AreAsyncReportsSettled(QueryTestHarness::CpuAddr, 4));
    REQUIRE(harness.rasterizer.pending.size() == 1);

    harness.rasterizer.pending.front()();
    harness.rasterizer.pending.clear();
    harness.cache.UnregisterPending();

    CHECK(harness.device_memory.Read<u32>(QueryTestHarness::CpuAddr) == Payload1 + 7U);
    CHECK(session1->AsyncReports().Completed(slot) == 1);
    CHECK(session1->AreAsyncReportsSettled(QueryTestHarness::CpuAddr, 4));
    CHECK(session1->SemaphoreWaiters().Latched(&waiter_owner));

    // Queue another deferred callback on session 1, then tear down session 1 before it runs.
    harness.cache.CounterReport(QueryTestHarness::GpuAddr, QueryType::Payload,
                                QueryPropertiesFlags::IsAFence, 0x200U, 0);
    REQUIRE(session1->AsyncReports().Issued(slot) == 2);
    REQUIRE(session1->AsyncReports().Completed(slot) == 1);
    REQUIRE(harness.rasterizer.pending.size() == 1);

    system.ShutdownMainProcess();
    CHECK(system.Nx2Session() == nullptr);
    CHECK(session1->IsRetired());
    CHECK(Core::Nx2SessionState::Lock(s1_handle) == nullptr);
    CHECK(session1->AsyncReports().Issued(slot) == 0);
    CHECK(session1->AsyncReports().Completed(slot) == 0);

    // Start Switch 2 session 2 before executing session 1's retired callback.
    system.SetPlatformProfile(Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch2));
    auto session2 = system.Nx2Session();
    REQUIRE(session2 != nullptr);
    REQUIRE(session2->GetHandle() != 0);
    REQUIRE(session2->GetHandle() != s1_handle);

    harness.rasterizer.pending.front()();
    harness.rasterizer.pending.clear();
    harness.cache.UnregisterPending();

    CHECK(session1->AsyncReports().Issued(slot) == 0);
    CHECK(session1->AsyncReports().Completed(slot) == 0);
    CHECK(session2->AsyncReports().Issued(slot) == 0);
    CHECK(session2->AsyncReports().Completed(slot) == 0);
    CHECK(session2->AreAsyncReportsSettled(QueryTestHarness::CpuAddr, 4));

    // Drop the last shared_ptr to session 2 after queueing a callback and verify safe retirement
    // even once the Nx2SessionState object has been destroyed.
    harness.cache.CounterReport(QueryTestHarness::GpuAddr, QueryType::Payload,
                                QueryPropertiesFlags::IsAFence, 0x300U, 0);
    REQUIRE(harness.rasterizer.pending.size() == 1);
    std::weak_ptr<Core::Nx2SessionState> s2_weak = session2;
    session2.reset();
    system.SetPlatformProfile(Core::MakePlatformProfile(Core::PlatformResolution::DefaultSwitch1));
    REQUIRE(s2_weak.expired());

    harness.rasterizer.pending.front()();
    harness.rasterizer.pending.clear();
    harness.cache.UnregisterPending();

    system.SetPlatformProfile(Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch2));
    auto session3 = system.Nx2Session();
    REQUIRE(session3 != nullptr);
    CHECK(session3->AsyncReports().Issued(slot) == 0);
    CHECK(session3->AsyncReports().Completed(slot) == 0);
}

TEST_CASE("Consecutive Switch 2 sessions start with clean waiters, counters, and memory state",
          "[core][nx2-session]") {
    ScopedSystemTestContext context;
    Core::System system;
    system.SetPlatformProfile(Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch2));
    system.Initialize();
    system.Kernel().Initialize();
    SCOPE_EXIT {
        system.ShutdownMainProcess();
        Settings::SetCurrentProgramIsSwitch2(false);
    };

    constexpr u64 SemAddr = 0x2000;
    constexpr u64 CpuRange = 0x80000000ULL;
    constexpr u64 GpuRange = 0xA0000000ULL;
    constexpr u64 SemRegion = 0xB0000000ULL;
    constexpr u64 SparseAddr = 0xC0000000ULL;
    const u16 slot = static_cast<u16>(Tegra::Nx2AsyncReports::Slot(SemAddr));

    int waiter_owner{};
    int listener_owner{};

    {
        auto session1 = system.Nx2Session();
        REQUIRE(session1 != nullptr);

        session1->SemaphoreWaiters().Add(&waiter_owner, SemAddr, 99,
                                         Tegra::Nx2SemaphoreWaiters::AcquireEqual);
        session1->SemaphoreWaiters().OnWrite(SemAddr, 99);
        REQUIRE(session1->SemaphoreWaiters().Size() == 1);
        REQUIRE(session1->SemaphoreWaiters().Latched(&waiter_owner));

        session1->IssueAsyncReport(slot);
        REQUIRE(session1->AsyncReports().Issued(slot) == 1);
        REQUIRE(session1->AsyncReports().Completed(slot) == 0);
        REQUIRE_FALSE(session1->AreAsyncReportsSettled(SemAddr, 16));

        auto& reg1 = session1->GpuMemoryRegistry();
        reg1.Register(CpuRange, 0x10000);
        reg1.RecordDeviceRange(CpuRange, 0x10000, 0x90000000ULL);
        reg1.CreateSparse(GpuRange, 0x100000);
        u32 allocs1 = 0;
        CHECK(reg1.AcquireDeviceRange(1, CpuRange, 0x10000, [&] {
            ++allocs1;
            return 0x91000000ULL;
        }) == 0x91000000ULL);
        CHECK(allocs1 == 1);

        session1->SemaphoreRegions().Add(SemRegion);
        CHECK(session1->SemaphoreRegions().Mirror(SemRegion + 0x1000) == SemRegion);

        session1->SparseState().RegionCreated(SparseAddr, 0x100000, 0x10000);
        session1->SparseState().AddSpan(SparseAddr, 0x100000);
        session1->SparseState().BlockWritten(SparseAddr, 0);
        CHECK(session1->SparseState().InRegion(SparseAddr, 0x10000));
    }

    system.ShutdownMainProcess();
    REQUIRE(system.Nx2Session() == nullptr);

    system.SetPlatformProfile(Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch2));
    system.Initialize();
    system.Kernel().Initialize();
    auto session2 = system.Nx2Session();
    REQUIRE(session2 != nullptr);

    CHECK(session2->SemaphoreWaiters().Size() == 0);
    CHECK_FALSE(session2->SemaphoreWaiters().Latched(&waiter_owner));

    CHECK(session2->AsyncReports().Issued(slot) == 0);
    CHECK(session2->AsyncReports().Completed(slot) == 0);
    CHECK(session2->AreAsyncReportsSettled(SemAddr, 16));

    auto& reg2 = session2->GpuMemoryRegistry();
    CHECK_FALSE(reg2.DeviceAddressOf(CpuRange, 1).has_value());
    u32 allocs2 = 0;
    CHECK(reg2.AcquireDeviceRange(1, CpuRange, 0x10000, [&] {
        ++allocs2;
        return 0x92000000ULL;
    }) == 0x92000000ULL);
    CHECK(allocs2 == 1);

    size_t replayed_mappings = 0;
    size_t replayed_sparse = 0;
    reg2.AddListener(
        &listener_owner,
        [&](u64, u64, u64) { ++replayed_mappings; },
        [](u64, u64) {},
        {},
        [&](u64, u64, bool) { ++replayed_sparse; });
    reg2.RemoveListener(&listener_owner);
    CHECK(replayed_mappings == 0);
    CHECK(replayed_sparse == 0);

    CHECK(session2->SemaphoreRegions().Mirror(SemRegion + 0x1000) == 0);
    CHECK_FALSE(session2->SparseState().InRegion(SparseAddr, 0x10000));
    CHECK(session2->SparseState().Low() == ~u64{0});
    CHECK(session2->SparseState().High() == 0);
}

namespace {

class SessionTestEmuWindow final : public Core::Frontend::EmuWindow {
public:
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<Core::Frontend::GraphicsContext>();
    }

    bool IsShown() const override {
        return false;
    }
};

template <typename T>
void WriteLeU64Or32(std::vector<u8>& buf, std::size_t offset, T value) {
    std::memcpy(buf.data() + offset, &value, sizeof(T));
}

} // namespace

TEST_CASE("Simultaneous Core::System instances isolate Switch 2 sessions and teardown",
          "[core][nx2-session]") {
    ScopedSystemTestContext context;

    Core::System system_a;
    system_a.SetPlatformProfile(
        Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch2));
    system_a.Initialize();
    system_a.Kernel().Initialize();

    Core::System system_b;
    system_b.SetPlatformProfile(
        Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch2));
    system_b.Initialize();
    system_b.Kernel().Initialize();

    Core::System system_c;
    system_c.SetPlatformProfile(
        Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch1));
    system_c.Initialize();
    system_c.Kernel().Initialize();

    SCOPE_EXIT {
        system_c.ShutdownMainProcess();
        system_b.ShutdownMainProcess();
        system_a.ShutdownMainProcess();
    };

    auto session_a = system_a.Nx2Session();
    auto session_b = system_b.Nx2Session();
    REQUIRE(session_a != nullptr);
    REQUIRE(session_b != nullptr);
    CHECK(session_a != session_b);
    CHECK(session_a->GetHandle() != session_b->GetHandle());
    CHECK(system_c.Nx2Session() == nullptr);

    constexpr u64 SemAddr = 0x3000ULL;
    constexpr u64 CpuRange = 0x80000000ULL;
    constexpr u64 DevRange = 0x95000000ULL;
    constexpr u64 SemRegion = 0xB4000000ULL;
    constexpr u64 SparseAddr = 0xC4000000ULL;
    const u16 slot = static_cast<u16>(Tegra::Nx2AsyncReports::Slot(SemAddr));
    int waiter_owner{};

    session_a->SemaphoreWaiters().Add(&waiter_owner, SemAddr, 77,
                                      Tegra::Nx2SemaphoreWaiters::AcquireEqual);
    session_a->SemaphoreWaiters().OnWrite(SemAddr, 77);
    session_a->IssueAsyncReport(slot);
    session_a->SemaphoreRegions().Add(SemRegion);
    session_a->SparseState().RegionCreated(SparseAddr, 0x100000, 0x10000);
    session_a->SparseState().AddSpan(SparseAddr, 0x100000);
    session_a->SparseState().BlockWritten(SparseAddr, 0);
    session_a->GpuMemoryRegistry().Register(CpuRange, 0x10000);
    session_a->GpuMemoryRegistry().RecordDeviceRange(CpuRange, 0x10000, DevRange);
    session_a->SetDriverActive(true);
    session_a->SetAmpere(true);
    CHECK(session_a->IncrementPresentCount() == 1);
    session_a->SetVideoLastFrameNs(123456789LL);

    Tegra::g_nx2_ampere.store(true, std::memory_order_relaxed);
    Service::Nvidia::Devices::g_nx2_driver.store(true, std::memory_order_relaxed);

    CHECK(session_b->SemaphoreWaiters().Size() == 0);
    CHECK_FALSE(session_b->SemaphoreWaiters().Latched(&waiter_owner));
    CHECK(session_b->AsyncReports().Issued(slot) == 0);
    CHECK(session_b->SemaphoreRegions().Mirror(SemRegion + 0x1000) == 0);
    CHECK(session_b->SparseState().Low() == ~u64{0});
    CHECK(session_b->SparseState().High() == 0);
    CHECK_FALSE(session_b->SparseState().InRegion(SparseAddr, 0x10000));
    CHECK_FALSE(session_b->GpuMemoryRegistry().DeviceAddressOf(CpuRange, 1).has_value());
    CHECK_FALSE(session_b->IsDriverActive());
    CHECK(session_b->PresentCount() == 0);
    CHECK(session_b->VideoLastFrameNs() == 0);

    // Shutting down another instance must leave session_a and active globals intact.
    system_c.ShutdownMainProcess();
    system_b.ShutdownMainProcess();
    CHECK(Core::Nx2SessionState::HasActiveSessions());
    CHECK(Tegra::g_nx2_ampere.load(std::memory_order_relaxed));
    CHECK(Service::Nvidia::Devices::g_nx2_driver.load(std::memory_order_relaxed));
    CHECK(session_a->SemaphoreWaiters().Latched(&waiter_owner));
    CHECK(session_a->GpuMemoryRegistry().DeviceAddressOf(CpuRange, 0x10000) == DevRange);
    CHECK(session_a->SparseState().InRegion(SparseAddr, 0x10000));

    system_a.ShutdownMainProcess();
    CHECK_FALSE(Core::Nx2SessionState::HasActiveSessions());
    CHECK_FALSE(Tegra::g_nx2_ampere.load(std::memory_order_relaxed));
    CHECK_FALSE(Service::Nvidia::Devices::g_nx2_driver.load(std::memory_order_relaxed));
    CHECK(Core::g_nx2_present_count.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("System lifecycle guards post-init profile changes and tears down without ShutdownMainProcess",
          "[core][nx2-session]") {
    ScopedSystemTestContext context;
    SessionTestEmuWindow window;

    SECTION("Post-kernel SetPlatformProfile is ignored and keeps kernel/memory consistent") {
        Core::System system;
        system.Initialize();
        system.Kernel().Initialize();
        SCOPE_EXIT {
            system.ShutdownMainProcess();
        };

        auto host1x = std::make_unique<Tegra::Host1x::Host1x>(system);
        REQUIRE(system.Platform().resolution == Core::PlatformResolution::DefaultSwitch1);
        REQUIRE(system.Kernel().ActiveCoreCount() == 4);
        REQUIRE(host1x->MemoryManager().DeviceAddressBits() == 34);
        REQUIRE(system.Nx2Session() == nullptr);

        system.SetPlatformProfile(
            Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch2));

        CHECK(system.Platform().resolution == Core::PlatformResolution::DefaultSwitch1);
        CHECK(system.Kernel().ActiveCoreCount() == 4);
        CHECK(host1x->MemoryManager().DeviceAddressBits() == 34);
        CHECK(system.Nx2Session() == nullptr);
    }

    SECTION("Pre-kernel SetPlatformProfile propagates DeviceAddressBits and ShutdownMainProcess resets shutdown flag") {
        Core::System system;
        system.InitializeGpuForTesting(window);
        CHECK(system.Host1x().MemoryManager().DeviceAddressBits() == 34);

        system.SetPlatformProfile(
            Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch2));
        CHECK(system.Host1x().MemoryManager().DeviceAddressBits() == 36);
        CHECK(system.Nx2Session() != nullptr);

        system.ShutdownMainProcess();
        CHECK(system.IsShuttingDown());
        CHECK_FALSE(Core::Nx2SessionState::HasActiveSessions());

        system.Initialize();
        CHECK_FALSE(system.IsShuttingDown());
        system.ShutdownMainProcess();
    }

    SECTION("Destroying System directly without ShutdownMainProcess tears down GPU and session safely") {
        std::weak_ptr<Core::Nx2SessionState> weak_session;
        {
            Core::System system;
            system.SetPlatformProfile(
                Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch2));
            system.Initialize();
            system.Kernel().Initialize();
            system.InitializeGpuForTesting(window);
            auto session = system.Nx2Session();
            REQUIRE(session != nullptr);
            weak_session = session;
            Tegra::g_nx2_ampere.store(true, std::memory_order_relaxed);
            Service::Nvidia::Devices::g_nx2_driver.store(true, std::memory_order_relaxed);
            Core::g_nx2_present_count.store(12, std::memory_order_relaxed);
        }
        CHECK(weak_session.expired());
        CHECK_FALSE(Core::Nx2SessionState::HasActiveSessions());
        CHECK_FALSE(Tegra::g_nx2_ampere.load(std::memory_order_relaxed));
        CHECK_FALSE(Service::Nvidia::Devices::g_nx2_driver.load(std::memory_order_relaxed));
        CHECK(Core::g_nx2_present_count.load(std::memory_order_relaxed) == 0);
    }
}

TEST_CASE("EnsureNx2Session attaches on DefaultSwitch1 and rejects ConfirmedSwitch1",
          "[core][nx2-session]") {
    ScopedSystemTestContext context;

    for (const auto rejected : {Core::PlatformResolution::ConfirmedSwitch1,
                                Core::PlatformResolution::OverrideSwitch1}) {
        Core::System system;
        system.SetPlatformProfile(Core::MakePlatformProfile(rejected));
        CHECK(system.Nx2Session() == nullptr);
        CHECK(system.EnsureNx2Session() == nullptr);
        CHECK(system.Nx2Session() == nullptr);
    }

    Core::System system;
    system.SetPlatformProfile(Core::MakePlatformProfile(Core::PlatformResolution::DefaultSwitch1));
    system.Initialize();
    system.Kernel().Initialize();
    SCOPE_EXIT {
        system.ShutdownMainProcess();
    };

    CHECK(system.Nx2Session() == nullptr);

    auto host1x = std::make_unique<Tegra::Host1x::Host1x>(system);
    Service::Nvidia::Module nvdrv{system, *host1x};
    Service::Nvidia::Ioctl ioctl{};
    ioctl.cmd.Assign(0x5);
    ioctl.group.Assign(static_cast<u32>('G'));
    ioctl.length.Assign(0x10);
    ioctl.is_in.Assign(1);
    ioctl.is_out.Assign(1);

    const auto fd = nvdrv.Open("/dev/nvhost-ctrl-gpu", Service::Nvidia::NvCore::SessionId{1});
    REQUIRE(fd != Service::Nvidia::INVALID_NVDRV_FD);
    SCOPE_EXIT {
        CHECK(nvdrv.Close(fd) == Service::Nvidia::NvResult::Success);
    };

    std::vector<u8> input(0xB0, 0);
    std::vector<u8> output(0xB0, 0);
    std::vector<u8> inline_output(0xD0, 0);
    WriteLeU64Or32<u64>(input, 0x00, 0xD0);

    REQUIRE(nvdrv.Ioctl3(fd, ioctl, input, output, inline_output) ==
            Service::Nvidia::NvResult::Success);

    const auto session = system.Nx2Session();
    REQUIRE(session != nullptr);
    CHECK(system.EnsureNx2Session() == session);
    CHECK(session->IsDriverActive());
    CHECK(session->IsAmpere());
}

TEST_CASE("Handle registry handles overflow beyond 64 sessions without collision and Retire cleans all state",
          "[core][nx2-session]") {
    constexpr std::size_t TotalSessions = 80;
    std::vector<std::shared_ptr<Core::Nx2SessionState>> sessions;
    std::vector<Core::Nx2SessionState::Handle> handles;
    std::unordered_set<Core::Nx2SessionState::Handle> unique_handles;
    std::size_t overflow_count = 0;

    sessions.reserve(TotalSessions);
    handles.reserve(TotalSessions);
    for (std::size_t i = 0; i < TotalSessions; ++i) {
        auto s = Core::Nx2SessionState::Create();
        REQUIRE(s != nullptr);
        const auto h = s->GetHandle();
        REQUIRE(h != 0);
        CHECK(unique_handles.insert(h).second);
        CHECK(Core::Nx2SessionState::Lock(h) == s);
        if ((h & 1ULL) == 0) {
            ++overflow_count;
        }
        sessions.push_back(std::move(s));
        handles.push_back(h);
    }
    CHECK(overflow_count >= TotalSessions - 64);
    CHECK(Core::Nx2SessionState::HasActiveSessions());

    // Populate every sub-registry on an overflow session and verify Retire clears all of it.
    auto target = sessions.back();
    const auto target_handle = target->GetHandle();
    const u16 slot = static_cast<u16>(Tegra::Nx2AsyncReports::Slot(0x4000ULL));
    int waiter_owner{};
    target->SemaphoreWaiters().Add(&waiter_owner, 0x4000ULL, 11,
                                   Tegra::Nx2SemaphoreWaiters::AcquireEqual);
    target->SemaphoreWaiters().OnWrite(0x4000ULL, 11);
    target->IssueAsyncReport(slot);
    target->SemaphoreRegions().Add(0xD0000000ULL);
    target->SparseState().RegionCreated(0xE0000000ULL, 0x100000, 0x10000);
    target->SparseState().AddSpan(0xE0000000ULL, 0x100000);
    target->SparseState().SetBacking([](u64, u64, const u8*) {});
    target->GpuMemoryRegistry().Register(0x88000000ULL, 0x10000);
    target->GpuMemoryRegistry().RecordDeviceRange(0x88000000ULL, 0x10000, 0x98000000ULL);
    target->SetDriverActive(true);
    target->SetAmpere(true);
    CHECK(target->IncrementPresentCount() == 1);
    target->SetVideoLastFrameNs(777);

    target->Retire();
    target->Retire();
    CHECK(target->IsRetired());
    CHECK(target->GetHandle() == 0);
    CHECK(Core::Nx2SessionState::Lock(target_handle) == nullptr);
    CHECK(target->SemaphoreWaiters().Size() == 0);
    CHECK_FALSE(target->SemaphoreWaiters().Latched(&waiter_owner));
    CHECK(target->AsyncReports().Issued(slot) == 0);
    CHECK(target->AsyncReports().Completed(slot) == 0);
    CHECK(target->SemaphoreRegions().Mirror(0xD0001000ULL) == 0);
    CHECK(target->SparseState().Low() == ~u64{0});
    CHECK(target->SparseState().High() == 0);
    CHECK_FALSE(target->SparseState().InRegion(0xE0000000ULL, 0x10000));
    CHECK_FALSE(target->GpuMemoryRegistry().DeviceAddressOf(0x88000000ULL, 1).has_value());
    CHECK_FALSE(target->IsDriverActive());
    CHECK_FALSE(target->IsAmpere());
    CHECK(target->PresentCount() == 0);
    CHECK(target->VideoLastFrameNs() == 0);

    // Expire a mix of fast and overflow handles, allocate replacements, and verify zero collisions.
    for (std::size_t i = 0; i < TotalSessions; i += 2) {
        sessions[i].reset();
    }
    for (std::size_t i = 0; i < 32; ++i) {
        auto replacement = Core::Nx2SessionState::Create();
        const auto h = replacement->GetHandle();
        REQUIRE(h != 0);
        CHECK(unique_handles.insert(h).second);
        CHECK(Core::Nx2SessionState::Lock(h) == replacement);
        sessions.push_back(std::move(replacement));
    }
    sessions.clear();
    target.reset();
    CHECK_FALSE(Core::Nx2SessionState::HasActiveSessions());
    for (const auto old_h : handles) {
        CHECK(Core::Nx2SessionState::Lock(old_h) == nullptr);
    }
}

TEST_CASE("SparseState and NX2 writer tracking handle backing install, bounds, wrap, and clear",
          "[core][nx2-session]") {
    ScopedSystemTestContext context;
    constexpr u64 MaxU64 = (std::numeric_limits<u64>::max)();

    Tegra::Nx2SparseState sparse;
    std::size_t backing_calls = 0;
    u64 last_write_addr = 0;
    sparse.RegionCreated(0x100000ULL, 0x200000ULL, 0x10000ULL);
    sparse.AddSpan(0x100000ULL, 0x200000ULL);
    REQUIRE(sparse.Low() == 0x100000ULL);
    REQUIRE(sparse.High() == 0x300000ULL);

    // Installing a non-empty backing handler preserves previously registered [Low, High] bounds.
    sparse.SetBacking([&](u64 addr, u64, const u8*) {
        ++backing_calls;
        last_write_addr = addr;
    });
    CHECK(sparse.Low() == 0x100000ULL);
    CHECK(sparse.High() == 0x300000ULL);

    const u32 word = 0xDEADBEEFU;
    sparse.BackWrite(0x100040ULL, sizeof(word), &word);
    CHECK(backing_calls == 1);
    CHECK(last_write_addr == 0x100040ULL);

    // Bounds-checked block writes and zero-sized/wrapping tile/span queries do not crash.
    sparse.BlockWritten(0x100000ULL, 999999ULL);
    CHECK_FALSE(sparse.BlockIsWritten(0x100000ULL, 0));
    sparse.RegionCreated(0x400000ULL, 0, 0x10000ULL);
    sparse.TilesMapped(0x400000ULL, 0, 0x1000ULL, true);
    CHECK_FALSE(sparse.OverlapsSpan(MaxU64 - 0x100ULL, 0x1000ULL));
    CHECK_FALSE(sparse.InRegion(MaxU64 - 0x100ULL, 0x1000ULL));
    CHECK_FALSE(sparse.Unwritten(MaxU64 - 0x100ULL, 0x1000ULL));

    // Clearing the backing resets Low/High bounds, and Clear() empties regions as well.
    sparse.SetBacking({});
    CHECK(sparse.Low() == ~u64{0});
    CHECK(sparse.High() == 0);
    sparse.Clear();
    CHECK_FALSE(sparse.InRegion(0x100000ULL, 0x10000ULL));

    // Global Nx2ClearSparseRegions resets g_nx2_sparse_low/high too.
    Tegra::Nx2AddSparseSpan(0x500000ULL, 0x10000ULL);
    CHECK(Tegra::g_nx2_sparse_low.load(std::memory_order_acquire) == 0x500000ULL);
    CHECK(Tegra::g_nx2_sparse_high.load(std::memory_order_acquire) == 0x510000ULL);
    Tegra::Nx2ClearSparseRegions();
    CHECK(Tegra::g_nx2_sparse_low.load(std::memory_order_acquire) == ~u64{0});
    CHECK(Tegra::g_nx2_sparse_high.load(std::memory_order_acquire) == 0);

    // Writer and proven-code tracking handle ranges/wraparound and Nx2ClearGpuWrittenPages clears all.
    Tegra::Nx2ClearGpuWrittenPages();
    Tegra::Nx2MarkGpuWritten(0x20000ULL, 0x2000ULL);
    CHECK(Tegra::Nx2IsGpuWritten(0x20000ULL));
    CHECK(Tegra::Nx2IsGpuWrittenRegion(0x20000ULL));
    Tegra::Nx2MarkProvenCode(0x2000ULL, MaxU64);
    CHECK(Tegra::Nx2IsProvenCode(0x2000ULL, 0x1000ULL));
    Tegra::Nx2MarkGraphicsWriterShader(0x40000U);
    CHECK(Tegra::Nx2IsGraphicsWriterShader(0x40000U));

    Tegra::Nx2ClearGpuWrittenPages();
    CHECK_FALSE(Tegra::Nx2IsGpuWritten(0x20000ULL));
    CHECK_FALSE(Tegra::Nx2IsGpuWrittenRegion(0x20000ULL));
    CHECK_FALSE(Tegra::Nx2IsProvenCode(0x2000ULL, 0x1000ULL));
    CHECK_FALSE(Tegra::Nx2IsGraphicsWriterShader(0x40000U));
}
