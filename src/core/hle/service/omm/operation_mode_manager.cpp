// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/omm/operation_mode_manager.h"
#include "core/hle/service/vi/vi_types.h"

namespace Service::OMM {

IOperationModeManager::IOperationModeManager(Core::System& system_)
    : ServiceFramework{system_, "omm"}, m_context{system_, "omm"},
      m_operation_mode_change_event{m_context},
      m_default_display_resolution_change_event{m_context},
      m_last_operation_mode_docked{Settings::IsDockedMode()},
      m_last_resolution_docked{Settings::IsDockedMode()} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IOperationModeManager::GetOperationMode>, "GetOperationMode"},
        {1, D<&IOperationModeManager::GetOperationModeChangeEvent>, "GetOperationModeChangeEvent"},
        {2, nullptr, "EnableAudioVisual"},
        {3, nullptr, "DisableAudioVisual"},
        {4, nullptr, "EnterSleepAndWait"},
        {5, D<&IOperationModeManager::GetCradleStatus>, "GetCradleStatus"},
        {6, nullptr, "FadeInDisplay"},
        {7, nullptr, "FadeOutDisplay"},
        {8, nullptr, "GetCradleFwVersion"},
        {9, nullptr, "NotifyCecSettingsChanged"},
        {10, nullptr, "SetOperationModePolicy"},
        {11, D<&IOperationModeManager::GetDefaultDisplayResolution>, "GetDefaultDisplayResolution"},
        {12, D<&IOperationModeManager::GetDefaultDisplayResolutionChangeEvent>, "GetDefaultDisplayResolutionChangeEvent"},
        {13, D<&IOperationModeManager::UpdateDefaultDisplayResolution>, "UpdateDefaultDisplayResolution"},
        {14, nullptr, "ShouldSleepOnBoot"},
        {15, nullptr, "NotifyHdcpApplicationExecutionStarted"},
        {16, nullptr, "NotifyHdcpApplicationExecutionFinished"},
        {17, nullptr, "NotifyHdcpApplicationDrawingStarted"},
        {18, nullptr, "NotifyHdcpApplicationDrawingFinished"},
        {19, nullptr, "GetHdcpAuthenticationFailedEvent"},
        {20, nullptr, "GetHdcpAuthenticationFailedEmulationEnabled"},
        {21, nullptr, "SetHdcpAuthenticationFailedEmulation"},
        {22, nullptr, "GetHdcpStateChangeEvent"},
        {23, nullptr, "GetHdcpState"},
        {24, nullptr, "ShowCardUpdateProcessing"},
        {25, nullptr, "SetApplicationCecSettingsAndNotifyChanged"},
        {26, D<&IOperationModeManager::GetOperationModeSystemInfo>, "GetOperationModeSystemInfo"},
        {27, nullptr, "GetAppletFullAwakingSystemEvent"},
        {28, nullptr, "CreateCradleFirmwareUpdater"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IOperationModeManager::~IOperationModeManager() = default;

void IOperationModeManager::NotifyOperationModeChanged() {
    std::scoped_lock lk{m_lock};
    const bool is_docked = Settings::IsDockedMode();
    if (is_docked != m_last_operation_mode_docked) {
        m_last_operation_mode_docked = is_docked;
        m_operation_mode_change_event.Signal(system.Kernel());
    }
}

Result IOperationModeManager::GetOperationMode(Out<OperationMode> out_operation_mode) {
    const bool is_docked = Settings::IsDockedMode();
    LOG_DEBUG(Service, "called, is_docked={}", is_docked);
    *out_operation_mode = is_docked ? OperationMode::Docked : OperationMode::Handheld;
    R_SUCCEED();
}

Result IOperationModeManager::GetOperationModeChangeEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service, "called");
    *out_event = m_operation_mode_change_event.GetHandle();
    R_SUCCEED();
}

Result IOperationModeManager::GetCradleStatus(Out<CradleStatus> out_cradle_status) {
    const bool is_docked = Settings::IsDockedMode();
    LOG_DEBUG(Service, "called, is_docked={}", is_docked);
    *out_cradle_status = is_docked ? CradleStatus::Docked : CradleStatus::Undocked;
    R_SUCCEED();
}

Result IOperationModeManager::GetDefaultDisplayResolution(Out<s32> out_width, Out<s32> out_height) {
    LOG_DEBUG(Service, "called");
    if (Settings::IsDockedMode()) {
        *out_width = static_cast<s32>(Service::VI::DisplayResolution::DockedWidth);
        *out_height = static_cast<s32>(Service::VI::DisplayResolution::DockedHeight);
    } else {
        *out_width = static_cast<s32>(Service::VI::DisplayResolution::UndockedWidth);
        *out_height = static_cast<s32>(Service::VI::DisplayResolution::UndockedHeight);
    }
    R_SUCCEED();
}

Result IOperationModeManager::GetDefaultDisplayResolutionChangeEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service, "called");
    *out_event = m_default_display_resolution_change_event.GetHandle();
    R_SUCCEED();
}

Result IOperationModeManager::UpdateDefaultDisplayResolution() {
    std::scoped_lock lk{m_lock};
    const bool is_docked = Settings::IsDockedMode();
    LOG_DEBUG(Service, "called, is_docked={}", is_docked);
    if (is_docked != m_last_resolution_docked) {
        m_last_resolution_docked = is_docked;
        m_default_display_resolution_change_event.Signal(system.Kernel());
    }
    R_SUCCEED();
}

Result IOperationModeManager::GetOperationModeSystemInfo(Out<u32> out_operation_mode_system_info) {
    LOG_DEBUG(Service, "called");
    *out_operation_mode_system_info = 0;
    R_SUCCEED();
}

} // namespace Service::OMM
