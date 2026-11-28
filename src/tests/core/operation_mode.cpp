// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "common/common_funcs.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/ipc.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/result.h"
#include "core/hle/service/am/am_types.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/lifecycle_manager.h"
#include "core/hle/service/am/service/common_state_getter.h"
#include "core/hle/service/apm/apm_controller.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/omm/operation_mode_manager.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"
#include "core/hle/service/vi/application_display_service.h"
#include "core/hle/service/vi/vi_types.h"
#include "tests/core/system_test_context.h"

namespace {

struct CmifResponse {
    Result result{ResultSuccess};
    std::array<u32, IPC::COMMAND_BUFFER_LENGTH> words{};
    u32 sfco_index{};
};

class OperationModeTestFixture {
public:
    explicit OperationModeTestFixture(Settings::ConsoleMode initial_mode)
        : prev_docked_mode{Settings::values.use_docked_mode.GetValue()} {
        Settings::values.use_docked_mode.SetValue(initial_mode);

        system.Initialize();
        auto& kernel = system.Kernel();
        bootstrap.emplace(kernel);
        REQUIRE(Kernel::KThread::InitializeDummyThread(system, &*bootstrap, nullptr).IsSuccess());
        kernel.SetCurrentEmuThread(&*bootstrap);
        kernel.Initialize();

        process.emplace(kernel);
        Kernel::KAutoObject::Create(&*process);

        client.emplace(kernel);
        Kernel::KAutoObject::Create(&*client);
        REQUIRE(Kernel::KThread::InitializeDummyThread(system, &*client, &*process).IsSuccess());

        server.emplace(system);
        server_thread.emplace(
            kernel.RunOnHostCoreProcess("op-mode-test-server", [&] { server->LoopProcess(); }));
        manager = std::make_shared<Service::SessionRequestManager>(kernel, *server);

        service_context.emplace(system, "op-mode-test");
        lifecycle.emplace(system, *service_context, true);
        // Match AppletManager::SetWindowSystem boot state for a foreground application.
        lifecycle->SetFocusState(kernel, Service::AM::FocusState::InFocus);
        last_notified_docked = Settings::IsDockedMode();

        // Commands 5, 6, 60 on ICommonStateGetter and 1102 on IApplicationDisplayService
        // read live console/display state without requiring a full ServiceManager boot.
        common_state_getter =
            std::make_shared<Service::AM::ICommonStateGetter>(system, nullptr);
        display_service =
            std::make_shared<Service::VI::IApplicationDisplayService>(system, nullptr);
        operation_mode_manager =
            std::make_shared<Service::OMM::IOperationModeManager>(system);
    }

    ~OperationModeTestFixture() {
        operation_mode_manager.reset();
        display_service.reset();
        common_state_getter.reset();
        lifecycle.reset();
        service_context.reset();

        manager.reset();
        server.reset();
        server_thread.reset();

        auto& kernel = system.Kernel();
        process->UnregisterThread(kernel, &*client);
        process->Close(kernel);
        kernel.UnregisterKernelObject(&*client);
        kernel.UnregisterKernelObject(&*process);

        system.CoreTiming().ClearPendingEvents();
        kernel.Shutdown();
        kernel.SetCurrentEmuThread(nullptr);
        kernel.UnregisterKernelObject(&*bootstrap);

        Settings::values.use_docked_mode.SetValue(prev_docked_mode);
    }

    OperationModeTestFixture(const OperationModeTestFixture&) = delete;
    OperationModeTestFixture& operator=(const OperationModeTestFixture&) = delete;

    CmifResponse InvokeCmif(Service::ServiceFrameworkBase& service, u32 command_id,
                            std::initializer_list<u32> args = {}) {
        std::array<u32, IPC::COMMAND_BUFFER_LENGTH> words{};
        IPC::CommandHeader header{};
        header.type.Assign(IPC::CommandType::Request);
        header.data_size.Assign(static_cast<u32>(4 + args.size() + 4));
        std::memcpy(words.data(), &header, sizeof(header));

        words[4] = Common::MakeMagic('S', 'F', 'C', 'I');
        words[5] = 0;
        words[6] = command_id;
        words[7] = 0;

        std::size_t idx = 8;
        for (const u32 arg : args) {
            words[idx++] = arg;
        }

        auto& kernel = system.Kernel();
        Service::HLERequestContext request{kernel, process->GetMemory(), nullptr, &*client};
        request.SetSessionRequestManager(manager);
        REQUIRE(request.PopulateFromIncomingCommandBuffer(words).IsSuccess());

        service.InvokeRequest(request);

        CmifResponse response{};
        std::memcpy(response.words.data(), request.CommandBuffer(),
                    response.words.size() * sizeof(u32));

        constexpr u32 SfcoMagic = Common::MakeMagic('S', 'F', 'C', 'O');
        bool found_sfco = false;
        for (u32 i = 0; i + 3 < response.words.size(); ++i) {
            if (response.words[i] == SfcoMagic) {
                response.sfco_index = i;
                response.result = Result{response.words[i + 2]};
                found_sfco = true;
                break;
            }
        }
        REQUIRE(found_sfco);
        return response;
    }

    Service::AM::OperationMode GetOperationMode() {
        const auto resp = InvokeCmif(*common_state_getter, 5);
        REQUIRE(resp.result.IsSuccess());
        return static_cast<Service::AM::OperationMode>(resp.words[resp.sfco_index + 4] & 0xFFU);
    }

    Service::APM::PerformanceMode GetPerformanceMode() {
        const auto resp = InvokeCmif(*common_state_getter, 6);
        REQUIRE(resp.result.IsSuccess());
        return static_cast<Service::APM::PerformanceMode>(resp.words[resp.sfco_index + 4]);
    }

    std::pair<s32, s32> GetDefaultDisplayResolution() {
        const auto resp = InvokeCmif(*common_state_getter, 60);
        REQUIRE(resp.result.IsSuccess());
        return {static_cast<s32>(resp.words[resp.sfco_index + 4]),
                static_cast<s32>(resp.words[resp.sfco_index + 5])};
    }

    std::pair<s64, s64> GetApplicationLogicalDisplayResolution(u64 display_id = 0) {
        s64 width{};
        s64 height{};
        REQUIRE(display_service->GetDisplayResolution(&width, &height, display_id).IsSuccess());
        return {width, height};
    }

    Service::OMM::OperationMode GetOmmOperationMode() {
        const auto resp = InvokeCmif(*operation_mode_manager, 0);
        REQUIRE(resp.result.IsSuccess());
        return static_cast<Service::OMM::OperationMode>(resp.words[resp.sfco_index + 4] & 0xFFU);
    }

    void CheckOmmCopyHandleCommand(u32 command_id) {
        const auto resp = InvokeCmif(*operation_mode_manager, command_id);
        REQUIRE(resp.result.IsSuccess());
        IPC::CommandHeader header{};
        std::memcpy(&header, resp.words.data(), sizeof(header));
        CHECK(header.enable_handle_descriptor.Value() == 1U);
        IPC::HandleDescriptorHeader handle_desc{};
        std::memcpy(&handle_desc, &resp.words[2], sizeof(handle_desc));
        CHECK(handle_desc.num_handles_to_copy.Value() == 1U);
        CHECK(handle_desc.num_handles_to_move.Value() == 0U);
    }

    Service::OMM::CradleStatus GetOmmCradleStatus() {
        const auto resp = InvokeCmif(*operation_mode_manager, 5);
        REQUIRE(resp.result.IsSuccess());
        return static_cast<Service::OMM::CradleStatus>(resp.words[resp.sfco_index + 4] & 0xFFU);
    }

    std::pair<s32, s32> GetOmmDefaultDisplayResolution() {
        const auto resp = InvokeCmif(*operation_mode_manager, 11);
        REQUIRE(resp.result.IsSuccess());
        return {static_cast<s32>(resp.words[resp.sfco_index + 4]),
                static_cast<s32>(resp.words[resp.sfco_index + 5])};
    }

    void UpdateOmmDefaultDisplayResolution() {
        const auto resp = InvokeCmif(*operation_mode_manager, 13);
        REQUIRE(resp.result.IsSuccess());
    }

    u32 GetOmmOperationModeSystemInfo() {
        const auto resp = InvokeCmif(*operation_mode_manager, 26);
        REQUIRE(resp.result.IsSuccess());
        return resp.words[resp.sfco_index + 4];
    }

    bool IsOmmOperationModeChangeEventSignaled() {
        return operation_mode_manager->GetOperationModeChangeEventHandle()->IsSignaled(
            system.Kernel());
    }

    void ResetOmmOperationModeChangeEvent() {
        REQUIRE(operation_mode_manager->GetOperationModeChangeEventHandle()
                    ->Reset(system.Kernel())
                    .IsSuccess());
    }

    bool IsOmmDefaultDisplayResolutionChangeEventSignaled() {
        return operation_mode_manager->GetDefaultDisplayResolutionChangeEventHandle()->IsSignaled(
            system.Kernel());
    }

    void ResetOmmDefaultDisplayResolutionChangeEvent() {
        REQUIRE(operation_mode_manager->GetDefaultDisplayResolutionChangeEventHandle()
                    ->Reset(system.Kernel())
                    .IsSuccess());
    }

    std::pair<bool, Service::AM::AppletMessage> PopMessage() {
        Service::AM::AppletMessage msg{Service::AM::AppletMessage::None};
        const bool ok = lifecycle->PopMessage(system.Kernel(), &msg);
        return {ok, msg};
    }

    void SetOperationModeChangedNotification(bool enabled) {
        lifecycle->SetOperationModeChangedNotificationEnabled(system.Kernel(), enabled);
    }

    void SetPerformanceModeChangedNotification(bool enabled) {
        lifecycle->SetPerformanceModeChangedNotificationEnabled(system.Kernel(), enabled);
    }

    bool IsSystemEventSignaled() {
        return lifecycle->GetSystemEvent().GetHandle()->IsSignaled(system.Kernel());
    }

    bool IsResolutionChangeEventSignaled() {
        return lifecycle->GetOperationModeChangedSystemEvent().GetHandle()->IsSignaled(
            system.Kernel());
    }

    void ResetResolutionChangeEvent() {
        REQUIRE(lifecycle->GetOperationModeChangedSystemEvent()
                    .GetHandle()
                    ->Reset(system.Kernel())
                    .IsSuccess());
    }

    void DrainInitialFocusMessage() {
        REQUIRE(IsSystemEventSignaled());
        const auto [ok, msg] = PopMessage();
        REQUIRE(ok);
        CHECK(msg == Service::AM::AppletMessage::FocusStateChanged);
        CHECK_FALSE(IsSystemEventSignaled());

        const auto [empty_ok, empty_msg] = PopMessage();
        CHECK_FALSE(empty_ok);
        CHECK(empty_msg == Service::AM::AppletMessage::None);
    }

    /// Mirrors AppletManager::OperationModeChanged() deduplication against the last mode.
    void ApplyConsoleMode(Settings::ConsoleMode mode) {
        Settings::values.use_docked_mode.SetValue(mode);
        // Also verify AppletManager::OperationModeChanged() is a safe no-op without WindowSystem.
        system.GetAppletManager().OperationModeChanged();

        const bool docked = Settings::IsDockedMode();
        if (docked != last_notified_docked) {
            last_notified_docked = docked;
            lifecycle->OnOperationAndPerformanceModeChanged(system.Kernel());
        }
    }

    ScopedSystemTestContext test_context{};
    const Settings::ConsoleMode prev_docked_mode;

    Core::System system{};
    std::optional<Kernel::KThread> bootstrap{};
    std::optional<Kernel::KProcess> process{};
    std::optional<Kernel::KThread> client{};
    std::optional<Service::ServerManager> server{};
    std::optional<std::jthread> server_thread{};
    std::shared_ptr<Service::SessionRequestManager> manager{};
    std::optional<Service::KernelHelpers::ServiceContext> service_context{};
    std::optional<Service::AM::LifecycleManager> lifecycle{};
    std::shared_ptr<Service::AM::ICommonStateGetter> common_state_getter{};
    std::shared_ptr<Service::VI::IApplicationDisplayService> display_service{};
    std::shared_ptr<Service::OMM::IOperationModeManager> operation_mode_manager{};
    bool last_notified_docked{};
};

} // namespace

TEST_CASE("Booting directly into docked or handheld reports matching mode and no spurious change",
          "[core][operation-mode]") {
    const auto initial_mode =
        GENERATE(Settings::ConsoleMode::Handheld, Settings::ConsoleMode::Docked);
    CAPTURE(initial_mode);

    OperationModeTestFixture fixture{initial_mode};
    const bool is_docked = initial_mode == Settings::ConsoleMode::Docked;

    CHECK(fixture.GetOperationMode() ==
          (is_docked ? Service::AM::OperationMode::Docked : Service::AM::OperationMode::Handheld));
    CHECK(fixture.GetPerformanceMode() == (is_docked ? Service::APM::PerformanceMode::Boost
                                                     : Service::APM::PerformanceMode::Normal));
    CHECK(fixture.GetDefaultDisplayResolution() ==
          (is_docked ? std::pair<s32, s32>{1920, 1080} : std::pair<s32, s32>{1280, 720}));

    // VI IApplicationDisplayService::GetDisplayResolution returns the fixed 1280x720 logical
    // resolution in both modes on Switch 1, distinct from AM GetDefaultDisplayResolution.
    CHECK(fixture.GetApplicationLogicalDisplayResolution() == std::pair<s64, s64>{1280, 720});

    fixture.DrainInitialFocusMessage();
    CHECK_FALSE(fixture.IsResolutionChangeEventSignaled());

    // Reapplying the boot mode must not emit a spurious transition notification.
    fixture.ApplyConsoleMode(initial_mode);
    CHECK_FALSE(fixture.IsSystemEventSignaled());
    CHECK_FALSE(fixture.IsResolutionChangeEventSignaled());
    const auto [ok, msg] = fixture.PopMessage();
    CHECK_FALSE(ok);
    CHECK(msg == Service::AM::AppletMessage::None);
}

TEST_CASE("Docked and handheld transitions notify in both directions and deduplicate repeats",
          "[core][operation-mode]") {
    OperationModeTestFixture fixture{Settings::ConsoleMode::Handheld};
    fixture.DrainInitialFocusMessage();

    // Handheld -> Docked
    fixture.ApplyConsoleMode(Settings::ConsoleMode::Docked);

    // Live queries already reflect the new mode before consuming notifications.
    CHECK(fixture.GetOperationMode() == Service::AM::OperationMode::Docked);
    CHECK(fixture.GetPerformanceMode() == Service::APM::PerformanceMode::Boost);
    CHECK(fixture.GetDefaultDisplayResolution() == std::pair<s32, s32>{1920, 1080});
    CHECK(fixture.GetApplicationLogicalDisplayResolution() == std::pair<s64, s64>{1280, 720});
    CHECK(fixture.IsSystemEventSignaled());
    CHECK(fixture.IsResolutionChangeEventSignaled());

    {
        const auto [ok1, msg1] = fixture.PopMessage();
        REQUIRE(ok1);
        CHECK(msg1 == Service::AM::AppletMessage::OperationModeChanged);
        // PerformanceModeChanged is still pending, so the system event stays signaled.
        CHECK(fixture.IsSystemEventSignaled());

        const auto [ok2, msg2] = fixture.PopMessage();
        REQUIRE(ok2);
        CHECK(msg2 == Service::AM::AppletMessage::PerformanceModeChanged);
        CHECK_FALSE(fixture.IsSystemEventSignaled());

        const auto [ok3, msg3] = fixture.PopMessage();
        CHECK_FALSE(ok3);
        CHECK(msg3 == Service::AM::AppletMessage::None);
    }

    // Mode queries after consuming messages match pre-consumption reads.
    CHECK(fixture.GetOperationMode() == Service::AM::OperationMode::Docked);
    CHECK(fixture.GetPerformanceMode() == Service::APM::PerformanceMode::Boost);
    CHECK(fixture.GetDefaultDisplayResolution() == std::pair<s32, s32>{1920, 1080});

    fixture.ResetResolutionChangeEvent();
    CHECK_FALSE(fixture.IsResolutionChangeEventSignaled());

    // Reapplying Docked must not produce a duplicate notification.
    fixture.ApplyConsoleMode(Settings::ConsoleMode::Docked);
    CHECK_FALSE(fixture.IsSystemEventSignaled());
    CHECK_FALSE(fixture.IsResolutionChangeEventSignaled());
    CHECK_FALSE(fixture.PopMessage().first);

    // Docked -> Handheld
    fixture.ApplyConsoleMode(Settings::ConsoleMode::Handheld);

    // Pre-consumption read reflects Handheld immediately.
    CHECK(fixture.GetOperationMode() == Service::AM::OperationMode::Handheld);
    CHECK(fixture.GetPerformanceMode() == Service::APM::PerformanceMode::Normal);
    CHECK(fixture.GetDefaultDisplayResolution() == std::pair<s32, s32>{1280, 720});
    CHECK(fixture.GetApplicationLogicalDisplayResolution() == std::pair<s64, s64>{1280, 720});
    CHECK(fixture.IsSystemEventSignaled());
    CHECK(fixture.IsResolutionChangeEventSignaled());

    {
        const auto [ok1, msg1] = fixture.PopMessage();
        REQUIRE(ok1);
        CHECK(msg1 == Service::AM::AppletMessage::OperationModeChanged);
        CHECK(fixture.IsSystemEventSignaled());

        const auto [ok2, msg2] = fixture.PopMessage();
        REQUIRE(ok2);
        CHECK(msg2 == Service::AM::AppletMessage::PerformanceModeChanged);
        CHECK_FALSE(fixture.IsSystemEventSignaled());

        CHECK_FALSE(fixture.PopMessage().first);
    }

    // Post-consumption read remains Handheld.
    CHECK(fixture.GetOperationMode() == Service::AM::OperationMode::Handheld);
    CHECK(fixture.GetPerformanceMode() == Service::APM::PerformanceMode::Normal);
    CHECK(fixture.GetDefaultDisplayResolution() == std::pair<s32, s32>{1280, 720});

    fixture.ResetResolutionChangeEvent();

    // Reapplying Handheld must not produce a duplicate notification.
    fixture.ApplyConsoleMode(Settings::ConsoleMode::Handheld);
    CHECK_FALSE(fixture.IsSystemEventSignaled());
    CHECK_FALSE(fixture.IsResolutionChangeEventSignaled());
    CHECK_FALSE(fixture.PopMessage().first);
}

TEST_CASE("Operation-mode and performance-mode notifications can be toggled independently",
          "[core][operation-mode]") {
    OperationModeTestFixture fixture{Settings::ConsoleMode::Handheld};
    fixture.DrainInitialFocusMessage();

    // Operation-mode enabled, performance-mode disabled.
    fixture.SetOperationModeChangedNotification(true);
    fixture.SetPerformanceModeChangedNotification(false);
    fixture.ApplyConsoleMode(Settings::ConsoleMode::Docked);

    CHECK(fixture.IsSystemEventSignaled());
    CHECK(fixture.IsResolutionChangeEventSignaled());
    {
        const auto [ok1, msg1] = fixture.PopMessage();
        REQUIRE(ok1);
        CHECK(msg1 == Service::AM::AppletMessage::OperationModeChanged);
        CHECK_FALSE(fixture.IsSystemEventSignaled());
        CHECK_FALSE(fixture.PopMessage().first);
    }
    fixture.ResetResolutionChangeEvent();

    // Operation-mode disabled, performance-mode enabled.
    fixture.SetOperationModeChangedNotification(false);
    fixture.SetPerformanceModeChangedNotification(true);
    fixture.ApplyConsoleMode(Settings::ConsoleMode::Handheld);

    CHECK(fixture.IsSystemEventSignaled());
    CHECK(fixture.IsResolutionChangeEventSignaled());
    {
        const auto [ok1, msg1] = fixture.PopMessage();
        REQUIRE(ok1);
        CHECK(msg1 == Service::AM::AppletMessage::PerformanceModeChanged);
        CHECK_FALSE(fixture.IsSystemEventSignaled());
        CHECK_FALSE(fixture.PopMessage().first);
    }
    fixture.ResetResolutionChangeEvent();

    // Both disabled: system event stays quiet, while the resolution change event still signals.
    fixture.SetOperationModeChangedNotification(false);
    fixture.SetPerformanceModeChangedNotification(false);
    fixture.ApplyConsoleMode(Settings::ConsoleMode::Docked);

    CHECK_FALSE(fixture.IsSystemEventSignaled());
    CHECK_FALSE(fixture.PopMessage().first);
    CHECK(fixture.IsResolutionChangeEventSignaled());
    CHECK(fixture.GetOperationMode() == Service::AM::OperationMode::Docked);
    CHECK(fixture.GetPerformanceMode() == Service::APM::PerformanceMode::Boost);
    fixture.ResetResolutionChangeEvent();

    // Re-enabling both does not retroactively deliver the transition that occurred while disabled.
    fixture.SetOperationModeChangedNotification(true);
    fixture.SetPerformanceModeChangedNotification(true);
    CHECK_FALSE(fixture.IsSystemEventSignaled());
    CHECK_FALSE(fixture.PopMessage().first);

    // Subsequent transition delivers both notifications in priority order again.
    fixture.ApplyConsoleMode(Settings::ConsoleMode::Handheld);
    CHECK(fixture.IsSystemEventSignaled());
    CHECK(fixture.IsResolutionChangeEventSignaled());
    {
        const auto [ok1, msg1] = fixture.PopMessage();
        REQUIRE(ok1);
        CHECK(msg1 == Service::AM::AppletMessage::OperationModeChanged);

        const auto [ok2, msg2] = fixture.PopMessage();
        REQUIRE(ok2);
        CHECK(msg2 == Service::AM::AppletMessage::PerformanceModeChanged);
        CHECK_FALSE(fixture.IsSystemEventSignaled());
        CHECK_FALSE(fixture.PopMessage().first);
    }
}

TEST_CASE("Back-to-back mode transitions before consumption coalesce into one notification pair",
          "[core][operation-mode]") {
    OperationModeTestFixture fixture{Settings::ConsoleMode::Handheld};
    fixture.DrainInitialFocusMessage();

    fixture.ApplyConsoleMode(Settings::ConsoleMode::Docked);
    fixture.ApplyConsoleMode(Settings::ConsoleMode::Handheld);

    CHECK(fixture.GetOperationMode() == Service::AM::OperationMode::Handheld);
    CHECK(fixture.GetPerformanceMode() == Service::APM::PerformanceMode::Normal);
    CHECK(fixture.IsSystemEventSignaled());

    const auto [ok1, msg1] = fixture.PopMessage();
    REQUIRE(ok1);
    CHECK(msg1 == Service::AM::AppletMessage::OperationModeChanged);

    const auto [ok2, msg2] = fixture.PopMessage();
    REQUIRE(ok2);
    CHECK(msg2 == Service::AM::AppletMessage::PerformanceModeChanged);
    CHECK_FALSE(fixture.IsSystemEventSignaled());

    CHECK_FALSE(fixture.PopMessage().first);
}

TEST_CASE("IOperationModeManager CMIF queries reflect docked and handheld state",
          "[core][operation-mode]") {
    const auto initial_mode =
        GENERATE(Settings::ConsoleMode::Handheld, Settings::ConsoleMode::Docked);
    CAPTURE(initial_mode);

    OperationModeTestFixture fixture{initial_mode};
    const bool is_docked = initial_mode == Settings::ConsoleMode::Docked;

    CHECK(fixture.GetOmmOperationMode() == (is_docked ? Service::OMM::OperationMode::Docked
                                                      : Service::OMM::OperationMode::Handheld));
    CHECK(fixture.GetOmmCradleStatus() ==
          (is_docked ? Service::OMM::CradleStatus::Docked : Service::OMM::CradleStatus::Undocked));
    CHECK(fixture.GetOmmDefaultDisplayResolution() ==
          (is_docked ? std::pair<s32, s32>{1920, 1080} : std::pair<s32, s32>{1280, 720}));
    CHECK(fixture.GetOmmOperationModeSystemInfo() == 0U);

    fixture.CheckOmmCopyHandleCommand(1);
    fixture.CheckOmmCopyHandleCommand(12);
    CHECK_FALSE(fixture.IsOmmOperationModeChangeEventSignaled());
    CHECK_FALSE(fixture.IsOmmDefaultDisplayResolutionChangeEventSignaled());

    // Calling UpdateDefaultDisplayResolution without a mode change must not signal spuriously.
    fixture.UpdateOmmDefaultDisplayResolution();
    fixture.operation_mode_manager->NotifyOperationModeChanged();
    CHECK_FALSE(fixture.IsOmmOperationModeChangeEventSignaled());
    CHECK_FALSE(fixture.IsOmmDefaultDisplayResolutionChangeEventSignaled());

    // Transition to the opposite mode and verify live queries and events update.
    const auto next_mode =
        is_docked ? Settings::ConsoleMode::Handheld : Settings::ConsoleMode::Docked;
    const bool next_docked = next_mode == Settings::ConsoleMode::Docked;
    Settings::values.use_docked_mode.SetValue(next_mode);

    CHECK(fixture.GetOmmOperationMode() == (next_docked ? Service::OMM::OperationMode::Docked
                                                        : Service::OMM::OperationMode::Handheld));
    CHECK(fixture.GetOmmCradleStatus() == (next_docked ? Service::OMM::CradleStatus::Docked
                                                       : Service::OMM::CradleStatus::Undocked));
    CHECK(fixture.GetOmmDefaultDisplayResolution() ==
          (next_docked ? std::pair<s32, s32>{1920, 1080} : std::pair<s32, s32>{1280, 720}));
    CHECK(fixture.GetOmmOperationModeSystemInfo() == 0U);

    fixture.UpdateOmmDefaultDisplayResolution();
    CHECK(fixture.IsOmmDefaultDisplayResolutionChangeEventSignaled());
    CHECK_FALSE(fixture.IsOmmOperationModeChangeEventSignaled());

    fixture.operation_mode_manager->NotifyOperationModeChanged();
    CHECK(fixture.IsOmmOperationModeChangeEventSignaled());

    fixture.ResetOmmDefaultDisplayResolutionChangeEvent();
    fixture.ResetOmmOperationModeChangeEvent();
    CHECK_FALSE(fixture.IsOmmDefaultDisplayResolutionChangeEventSignaled());
    CHECK_FALSE(fixture.IsOmmOperationModeChangeEventSignaled());

    // Repeating updates in the same mode must not re-signal either event.
    fixture.UpdateOmmDefaultDisplayResolution();
    fixture.operation_mode_manager->NotifyOperationModeChanged();
    CHECK_FALSE(fixture.IsOmmDefaultDisplayResolutionChangeEventSignaled());
    CHECK_FALSE(fixture.IsOmmOperationModeChangeEventSignaled());
}

TEST_CASE("LifecycleManager matches nn::oe message IDs, resume opt-in, and focus handling",
          "[core][operation-mode]") {
    CHECK(static_cast<u32>(Service::AM::AppletMessage::Exit) == 4U);
    CHECK(static_cast<u32>(Service::AM::AppletMessage::FocusStateChanged) == 15U);
    CHECK(static_cast<u32>(Service::AM::AppletMessage::Resume) == 16U);
    CHECK(static_cast<u32>(Service::AM::AppletMessage::OperationModeChanged) == 30U);
    CHECK(static_cast<u32>(Service::AM::AppletMessage::PerformanceModeChanged) == 31U);

    OperationModeTestFixture fixture{Settings::ConsoleMode::Handheld};
    fixture.DrainInitialFocusMessage();

    // Resume notification is disabled by default until opted in.
    fixture.lifecycle->RequestResumeNotification();
    fixture.lifecycle->SignalSystemEventIfNeeded(fixture.system.Kernel());
    CHECK_FALSE(fixture.IsSystemEventSignaled());
    CHECK_FALSE(fixture.PopMessage().first);

    fixture.lifecycle->SetResumeNotificationEnabled(true);
    fixture.lifecycle->RequestResumeNotification();
    fixture.lifecycle->SignalSystemEventIfNeeded(fixture.system.Kernel());
    CHECK(fixture.IsSystemEventSignaled());
    {
        const auto [ok, msg] = fixture.PopMessage();
        REQUIRE(ok);
        CHECK(msg == Service::AM::AppletMessage::Resume);
        CHECK_FALSE(fixture.IsSystemEventSignaled());
    }

    // Default FocusHandlingMode (SuspendHomeSleep) suspends in background, runs if obscured.
    CHECK(fixture.lifecycle->IsRunnable());
    fixture.lifecycle->SetActivityState(Service::AM::ActivityState::ForegroundObscured);
    CHECK(fixture.lifecycle->IsRunnable());
    fixture.lifecycle->SetActivityState(Service::AM::ActivityState::BackgroundVisible);
    CHECK_FALSE(fixture.lifecycle->IsRunnable());

    // Out-of-focus suspending (AlwaysSuspend) pauses even when ForegroundObscured.
    fixture.lifecycle->SetActivityState(Service::AM::ActivityState::ForegroundObscured);
    fixture.lifecycle->SetOutOfFocusSuspendingEnabled(true);
    CHECK_FALSE(fixture.lifecycle->IsRunnable());
    fixture.lifecycle->SetOutOfFocusSuspendingEnabled(false);
    CHECK(fixture.lifecycle->IsRunnable());

    // Disabling suspend (NoSuspend) keeps the application runnable in background.
    fixture.lifecycle->SetActivityState(Service::AM::ActivityState::BackgroundVisible);
    fixture.lifecycle->SetFocusHandlingMode(false);
    CHECK(fixture.lifecycle->IsRunnable());
    CHECK(fixture.lifecycle->UpdateRequestedFocusState());
    CHECK(fixture.lifecycle->GetAndClearFocusState() == Service::AM::FocusState::Background);

    // Disabling focus-changed notifications suppresses FocusStateChanged delivery.
    fixture.lifecycle->SetFocusStateChangedNotificationEnabled(fixture.system.Kernel(), false);
    CHECK_FALSE(fixture.IsSystemEventSignaled());
    CHECK_FALSE(fixture.PopMessage().first);

    // Exit request always makes the applet runnable and delivers Exit once.
    fixture.lifecycle->SetFocusHandlingMode(true);
    CHECK_FALSE(fixture.lifecycle->IsRunnable());
    fixture.lifecycle->RequestExit(fixture.system.Kernel());
    CHECK(fixture.lifecycle->IsRunnable());
    CHECK(fixture.IsSystemEventSignaled());
    {
        const auto [ok, msg] = fixture.PopMessage();
        REQUIRE(ok);
        CHECK(msg == Service::AM::AppletMessage::Exit);
        CHECK_FALSE(fixture.IsSystemEventSignaled());
        CHECK_FALSE(fixture.PopMessage().first);
    }
}
