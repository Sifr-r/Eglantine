// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/file_sys/program_metadata.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/host1x/syncpoint_manager.h"
#include "video_core/nx2.h"

TEST_CASE("SyncpointManager supports multiple keyed guest mirrors ordered before wait actions",
          "[nx2_concurrency]") {
    Tegra::Host1x::SyncpointManager manager;

    CHECK(manager.RegisterGuestMirror({}) ==
          Tegra::Host1x::SyncpointManager::InvalidGuestMirrorHandle);
    manager.UnregisterGuestMirror(Tegra::Host1x::SyncpointManager::InvalidGuestMirrorHandle);

    std::vector<std::pair<u32, u32>> seen_a;
    std::vector<std::pair<u32, u32>> seen_b;
    std::vector<int> order;

    const auto handle_a = manager.RegisterGuestMirror([&](u32 id, u32 value) {
        seen_a.emplace_back(id, value);
        order.push_back(1);
    });
    const auto handle_b = manager.RegisterGuestMirror([&](u32 id, u32 value) {
        seen_b.emplace_back(id, value);
        order.push_back(2);
    });
    REQUIRE(handle_a != Tegra::Host1x::SyncpointManager::InvalidGuestMirrorHandle);
    REQUIRE(handle_b != Tegra::Host1x::SyncpointManager::InvalidGuestMirrorHandle);
    REQUIRE(handle_a != handle_b);

    constexpr u32 syncpt = 7;
    manager.RegisterGuestAction(syncpt, 1, [&] { order.push_back(3); });
    manager.IncrementGuest(syncpt);

    REQUIRE(seen_a.size() == 1);
    REQUIRE(seen_b.size() == 1);
    CHECK(seen_a[0] == std::pair<u32, u32>{syncpt, 1});
    CHECK(seen_b[0] == std::pair<u32, u32>{syncpt, 1});
    REQUIRE(order.size() == 3);
    CHECK(order[2] == 3);

    manager.UnregisterGuestMirror(handle_a);
    manager.UnregisterGuestMirror(handle_a);
    manager.IncrementGuest(syncpt);

    CHECK(seen_a.size() == 1);
    REQUIRE(seen_b.size() == 2);
    CHECK(seen_b[1] == std::pair<u32, u32>{syncpt, 2});

    std::atomic<bool> stop{false};
    std::thread incrementer([&] {
        while (!stop.load(std::memory_order_acquire)) {
            manager.IncrementGuest(syncpt);
        }
    });

    for (int i = 0; i < 64; ++i) {
        auto state = std::make_shared<std::atomic<bool>>(true);
        std::atomic<bool> called_after_unregister{false};
        const auto h = manager.RegisterGuestMirror([state, &called_after_unregister](u32, u32) {
            if (!state->load(std::memory_order_acquire)) {
                called_after_unregister.store(true, std::memory_order_release);
            }
        });
        std::this_thread::yield();
        manager.UnregisterGuestMirror(h);
        state->store(false, std::memory_order_release);
        REQUIRE_FALSE(called_after_unregister.load(std::memory_order_acquire));
    }

    stop.store(true, std::memory_order_release);
    incrementer.join();
    manager.UnregisterGuestMirror(handle_b);
}

TEST_CASE("Nx2SetSparseBacking drains in-flight callbacks and allows reentrant writes",
          "[nx2_concurrency]") {
    SCOPE_EXIT {
        Tegra::Nx2SetSparseBacking({});
    };

    std::atomic<u32> reentrant_invocations{0};
    Tegra::Nx2SetSparseBacking([&](u64 gpu_addr, u64 size, const u8*) {
        const u32 count = reentrant_invocations.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count == 1) {
            const u32 nested_word = 0x12345678U;
            Tegra::Nx2BackSparseWrite(gpu_addr + size, sizeof(nested_word), &nested_word);
        }
    });
    Tegra::Nx2AddSparseSpan(0x10000, 0x2000);
    const u32 initial_payload = 0xAABBCCDDU;
    Tegra::Nx2BackSparseWrite(0x10000, sizeof(initial_payload), &initial_payload);
    CHECK(reentrant_invocations.load(std::memory_order_acquire) == 2);

    std::mutex mu;
    std::condition_variable cv;
    bool handler_entered = false;
    std::atomic<bool> handler_finished{false};

    Tegra::Nx2SetSparseBacking([&](u64, u64, const u8*) {
        {
            std::scoped_lock lk{mu};
            handler_entered = true;
        }
        cv.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        handler_finished.store(true, std::memory_order_release);
    });
    Tegra::Nx2AddSparseSpan(0x10000, 0x2000);

    std::thread writer(
        [&] { Tegra::Nx2BackSparseWrite(0x10000, sizeof(initial_payload), &initial_payload); });

    {
        std::unique_lock lk{mu};
        cv.wait(lk, [&] { return handler_entered; });
    }
    Tegra::Nx2SetSparseBacking({});
    CHECK(handler_finished.load(std::memory_order_acquire));
    writer.join();
}

TEST_CASE("DeviceMemoryManager::CanTrapCpuReads tracks fastmem registration and NCE/reactive settings",
          "[nx2_concurrency]") {
    const auto saved_backend = Settings::values.cpu_backend.GetValue();
    const auto saved_reactive = Settings::values.use_reactive_flushing.GetValue();
    SCOPE_EXIT {
        Settings::values.cpu_backend.SetValue(saved_backend);
        Settings::values.use_reactive_flushing.SetValue(saved_reactive);
    };

    Settings::values.cpu_backend.SetValue(Settings::CpuBackend::Dynarmic);
    Settings::values.use_reactive_flushing.SetValue(false);

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
    REQUIRE(process->IsApplication());
    process->GetMemory().SetCurrentPageTable(*process);
    REQUIRE(process->GetMemory().HasFastmemArena());

    Tegra::MaxwellDeviceMemoryManager device_memory{system.DeviceMemory()};
    CHECK_FALSE(device_memory.CanTrapCpuReads());

    const auto asid = device_memory.RegisterProcess(&process->GetMemory());
    CHECK(device_memory.CanTrapCpuReads());

    Settings::values.cpu_backend.SetValue(Settings::CpuBackend::Nce);
    CHECK_FALSE(device_memory.CanTrapCpuReads());

    Settings::values.cpu_backend.SetValue(Settings::CpuBackend::Dynarmic);
    Settings::values.use_reactive_flushing.SetValue(true);
    CHECK_FALSE(device_memory.CanTrapCpuReads());

    Settings::values.use_reactive_flushing.SetValue(false);
    CHECK(device_memory.CanTrapCpuReads());

    device_memory.UnregisterProcess(asid);
    CHECK_FALSE(device_memory.CanTrapCpuReads());
}
