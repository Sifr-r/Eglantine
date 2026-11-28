// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/os/event.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Kernel {
class KReadableEvent;
}

namespace Service::OMM {

enum class OperationMode : u8 {
    Handheld = 0,
    Docked = 1,
};

enum class CradleStatus : u8 {
    Undocked = 0,
    Docked = 1,
};

class IOperationModeManager final : public ServiceFramework<IOperationModeManager> {
public:
    explicit IOperationModeManager(Core::System& system_);
    ~IOperationModeManager() override;

    Kernel::KReadableEvent* GetOperationModeChangeEventHandle() noexcept {
        return m_operation_mode_change_event.GetHandle();
    }

    Kernel::KReadableEvent* GetDefaultDisplayResolutionChangeEventHandle() noexcept {
        return m_default_display_resolution_change_event.GetHandle();
    }

    void NotifyOperationModeChanged();

private:
    Result GetOperationMode(Out<OperationMode> out_operation_mode);
    Result GetOperationModeChangeEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetCradleStatus(Out<CradleStatus> out_cradle_status);
    Result GetDefaultDisplayResolution(Out<s32> out_width, Out<s32> out_height);
    Result GetDefaultDisplayResolutionChangeEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result UpdateDefaultDisplayResolution();
    Result GetOperationModeSystemInfo(Out<u32> out_operation_mode_system_info);

    KernelHelpers::ServiceContext m_context;
    Event m_operation_mode_change_event;
    Event m_default_display_resolution_change_event;
    std::mutex m_lock{};
    bool m_last_operation_mode_docked{};
    bool m_last_resolution_docked{};
};

} // namespace Service::OMM
