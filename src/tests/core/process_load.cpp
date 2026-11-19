// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "core/core.h"
#include "core/file_sys/program_metadata.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/os/process.h"
#include "core/loader/loader.h"
#include "tests/core/system_test_context.h"

namespace {

enum class LoadOutcome {
    FailBeforeInitialization,
    FailAfterInitialization,
    Succeed,
};

class ProcessTestLoader final : public Loader::AppLoader {
public:
    explicit ProcessTestLoader(LoadOutcome outcome) : AppLoader(nullptr), outcome{outcome} {}

    Loader::FileType GetFileType() const override {
        return Loader::FileType::Unknown;
    }

    LoadResult Load(Kernel::KProcess& process, Core::System& system) override {
        slab_address = reinterpret_cast<uintptr_t>(&process);
        if (outcome != LoadOutcome::FailBeforeInitialization) {
            const auto metadata = FileSys::ProgramMetadata::GetDefault();
            REQUIRE(process.LoadFromMetadata(system.Kernel(), metadata, Kernel::PageSize, {}, 0)
                        .IsSuccess());
            REQUIRE(process.IsInitialized());
        }
        if (outcome == LoadOutcome::Succeed) {
            return {Loader::ResultStatus::Success, LoadParameters{0x2c, 0x100000}};
        }
        return {Loader::ResultStatus::ErrorLoadingNSO, std::nullopt};
    }

    uintptr_t slab_address{};

private:
    const LoadOutcome outcome;
};

} // namespace

TEST_CASE("Failed application loads release their kernel process", "[core][process-load]") {
    const ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    kernel.Initialize();
    SCOPE_EXIT { system.ShutdownMainProcess(); };

    Service::Process process{system};
    const auto registered_objects = kernel.ObjectListContainer().GetOwnedCount(nullptr);
    Loader::ResultStatus status = Loader::ResultStatus::Success;
    auto outcome = LoadOutcome::FailBeforeInitialization;

    SECTION("before kernel process initialization") {}
    SECTION("after kernel process initialization") {
        outcome = LoadOutcome::FailAfterInitialization;
    }

    ProcessTestLoader failing_loader{outcome};
    REQUIRE_FALSE(process.Initialize(failing_loader, status));
    CHECK(status == Loader::ResultStatus::ErrorLoadingNSO);
    CHECK_FALSE(process.IsInitialized());
    CHECK(kernel.GetProcessList().empty());
    REQUIRE(kernel.ObjectListContainer().GetOwnedCount(nullptr) == registered_objects);

    ProcessTestLoader successful_loader{LoadOutcome::Succeed};
    REQUIRE(process.Initialize(successful_loader, status));
    CHECK(status == Loader::ResultStatus::Success);
    REQUIRE(process.IsInitialized());
    CHECK(successful_loader.slab_address == failing_loader.slab_address);
    {
        Kernel::KScopedSchedulerLock lock(kernel);
        CHECK_FALSE(process.GetHandle()->IsSignaled(kernel));
    }
    CHECK(kernel.GetProcessByProcessId(process.GetProcessId()).GetPointerUnsafe() ==
          process.GetHandle());
    CHECK(kernel.ObjectListContainer().GetOwnedCount(nullptr) == registered_objects + 1);

    process.Finalize();
    CHECK(kernel.GetProcessList().empty());
    CHECK(kernel.ObjectListContainer().GetOwnedCount(nullptr) == registered_objects);
}
