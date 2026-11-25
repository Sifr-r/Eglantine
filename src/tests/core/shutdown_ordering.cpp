// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <stop_token>
#include <thread>

#include "common/scope_exit.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/file_sys/program_metadata.h"
#include "core/hle/kernel/k_auto_object_container.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/nx2_gsv.h"
#include "core/hle/service/os/process.h"
#include "core/loader/loader.h"
#include "core/memory/cheat_engine.h"
#include "tests/core/system_test_context.h"

namespace {

/// Brings a guest process up far enough for the kernel to take ownership of it.
class TestProcessLoader final : public Loader::AppLoader {
public:
    TestProcessLoader() : AppLoader(nullptr) {}

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

} // Anonymous namespace

TEST_CASE("ShutdownMainProcess keeps the timing thread paused", "[core][shutdown-ordering]") {
    const ScopedSystemTestContext context;
    // The timing thread only exists when the emulation runs on more than one core.
    Settings::values.use_multi_core.SetValue(true);

    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    kernel.Initialize();

    auto& core_timing = system.CoreTiming();

    // The timing thread is the one that runs the event callbacks. It has to be running when
    // the shutdown starts, otherwise the shutdown cannot restart it.
    core_timing.SyncPause(false);
    REQUIRE(core_timing.IsRunning());

    const auto session_stop = system.GetEmulationStopToken();
    bool timing_running_at_stop = false;
    const std::stop_callback on_stop(session_stop,
                                     [&] { timing_running_at_stop = core_timing.IsRunning(); });

    system.ShutdownMainProcess();

    CHECK(session_stop.stop_requested());
    CHECK(timing_running_at_stop);
    const auto next_session_stop = system.GetEmulationStopToken();
    CHECK(next_session_stop.stop_possible());
    CHECK_FALSE(next_session_stop.stop_requested());

    // The teardown destroys the services, the guest processes and the cheat engine. A timing
    // callback running at that point uses freed objects, so the timing thread has to stay
    // paused for the whole teardown.
    CHECK_FALSE(core_timing.IsRunning());
}

TEST_CASE("Cheat callbacks do not use a process the kernel released",
          "[core][shutdown-ordering]") {
    const ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    kernel.Initialize();
    SCOPE_EXIT { system.ShutdownMainProcess(); };

    Service::Process process{system};
    Loader::ResultStatus status = Loader::ResultStatus::Success;
    TestProcessLoader loader;
    REQUIRE(process.Initialize(loader, status));
    REQUIRE(process.IsInitialized());

    // Hold a reference to the process, so that it stays alive after the kernel released it.
    // The test then observes the state of a process that the cheat callbacks must not touch.
    Kernel::KScopedAutoObject<Kernel::KProcess> handle =
        kernel.GetProcessByProcessId(process.GetProcessId());
    REQUIRE(handle.IsNotNull());

    Core::Memory::CheatProcessMetadata metadata{};
    metadata.process_id = process.GetProcessId();
    Core::Memory::StandardVmCallbacks callbacks{system, metadata};

    // Reach the process while the kernel still owns it. The cheat callbacks used to cache the
    // process pointer here.
    std::array<u8, 16> buffer{};
    callbacks.MemoryReadUnsafe(0, buffer.data(), buffer.size());

    // The kernel releases the process: it does not know about it any more.
    kernel.RemoveProcess(handle.GetPointerUnsafe());
    REQUIRE(kernel.GetProcessList().empty());

    // The callbacks resolve the process through the kernel on every use, so they do not touch
    // the process the kernel released. A callback that used a cached pointer would have paused
    // it here.
    callbacks.PauseProcess();
    CHECK_FALSE(handle->IsSuspended());
    callbacks.ResumeProcess();
    CHECK_FALSE(handle->IsSuspended());
}

TEST_CASE("The kernel empties its object list before the guest memory goes away",
          "[core][shutdown-ordering]") {
    const ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    kernel.Initialize();

    // The objects in the kernel's object list live in the guest RAM arena. The system frees
    // that arena before it destroys the kernel, so the list has to be emptied while the arena
    // is still alive: a container that walks its list afterwards reads freed memory.
    REQUIRE(kernel.ObjectListContainer().GetOwnedCount(nullptr) > 0);
    kernel.Shutdown();
    CHECK(kernel.ObjectListContainer().GetOwnedCount(nullptr) == 0);

    // The container itself stays alive after the shutdown, so an object that is closed later
    // still finds it and unregisters itself from it.
    CHECK_NOTHROW(kernel.ObjectListContainer().GetOwnedCount(nullptr));
}

TEST_CASE("The kernel object list survives a system that is destroyed without a shutdown",
          "[core][shutdown-ordering]") {
    const ScopedSystemTestContext context;

    // The system is destroyed without a shutdown: the kernel's object list still holds the
    // objects that live in the guest RAM arena, and that arena is gone by the time the kernel
    // is destroyed. The teardown has to empty the list first.
    REQUIRE_NOTHROW([] {
        Core::System system;
        system.Initialize();
        auto& kernel = system.Kernel();
        kernel.Initialize();
        REQUIRE(kernel.ObjectListContainer().GetOwnedCount(nullptr) > 0);
    }());
}

TEST_CASE("KAutoObjectWithListContainer Finalize is idempotent and refuses post-finalize registration",
          "[core][shutdown-ordering]") {
    const ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();

    Kernel::KAutoObjectWithListContainer container{kernel};
    container.Initialize();
    SCOPE_EXIT {
        container.Finalize();
    };

    Kernel::KEvent event_a{kernel};
    Kernel::KEvent event_b{kernel};

    container.Register(&event_a);
    CHECK(event_a.is_linked());
    CHECK(container.GetOwnedCount(nullptr) == 1);

    container.Finalize();
    CHECK_FALSE(event_a.is_linked());
    CHECK(container.GetOwnedCount(nullptr) == 0);

    container.Finalize();
    CHECK(container.GetOwnedCount(nullptr) == 0);

    container.Register(&event_b);
    CHECK_FALSE(event_b.is_linked());
    CHECK(container.GetOwnedCount(nullptr) == 0);

    container.Unregister(&event_a);
    container.Unregister(&event_b);
    CHECK_FALSE(event_a.is_linked());
    CHECK_FALSE(event_b.is_linked());
    CHECK(container.GetOwnedCount(nullptr) == 0);
}

TEST_CASE("Kernel shutdown closes core main, idle, and shutdown threads after ShutdownCores",
          "[core][shutdown-ordering]") {
    const ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    kernel.Initialize();

    REQUIRE(kernel.ObjectListContainer().GetOwnedCount(nullptr) > 0);
    kernel.ShutdownCores();
    kernel.Shutdown();
    CHECK(kernel.ObjectListContainer().GetOwnedCount(nullptr) == 0);

    // Calling Shutdown() again must remain safe without double-closing core threads.
    CHECK_NOTHROW(kernel.Shutdown());
}

TEST_CASE("DrainGpuCallbacks wakes early on stop token and bounds wedged GPU waits",
          "[core][shutdown-ordering]") {
    SECTION("Pre-stopped token skips queueing and returns immediately") {
        std::stop_source stop_source;
        stop_source.request_stop();
        bool enqueued = false;

        const bool ok = Service::Detail::WaitForGpuCallbackDrain(
            stop_source.get_token(), std::chrono::seconds{5},
            [&](auto&&) { enqueued = true; }, [] { return false; });

        CHECK(ok);
        CHECK_FALSE(enqueued);
    }

    SECTION("Stop token wakes a wedged wait early") {
        std::stop_source stop_source;
        std::jthread stopper([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            stop_source.request_stop();
        });

        const auto start = std::chrono::steady_clock::now();
        const bool ok = Service::Detail::WaitForGpuCallbackDrain(
            stop_source.get_token(), std::chrono::seconds{5}, [](auto&&) {}, [] { return false; });
        const auto elapsed = std::chrono::steady_clock::now() - start;

        CHECK(ok);
        CHECK(elapsed < std::chrono::seconds{2});
    }

    SECTION("Completed callback finishes and returns true") {
        std::stop_source stop_source;
        std::jthread worker;
        const bool ok = Service::Detail::WaitForGpuCallbackDrain(
            stop_source.get_token(), std::chrono::seconds{5},
            [&](auto cb) { worker = std::jthread(std::move(cb)); }, [] { return false; });

        CHECK(ok);
    }

    SECTION("Wedged callback without stop hits the bounded timeout and returns false") {
        std::stop_source stop_source;
        const bool ok = Service::Detail::WaitForGpuCallbackDrain(
            stop_source.get_token(), std::chrono::milliseconds{15}, [](auto&&) {},
            [] { return false; });

        CHECK_FALSE(ok);
    }
}
