// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/os/event.h"
#include "core/hle/service/vi/vsync_manager.h"

namespace Service::VI {

VsyncManager::VsyncManager() = default;
VsyncManager::~VsyncManager() = default;

void VsyncManager::SignalVsync(Kernel::KernelCore& kernel) {
    std::scoped_lock lk{m_lock};
    for (auto* event : m_vsync_events) {
        event->Signal(kernel);
    }
}

void VsyncManager::LinkVsyncEvent(Event* event) {
    if (event == nullptr) {
        return;
    }
    std::scoped_lock lk{m_lock};
    m_vsync_events.insert(event);
}

void VsyncManager::UnlinkVsyncEvent(Event* event) {
    std::scoped_lock lk{m_lock};
    m_vsync_events.erase(event);
}

} // namespace Service::VI
