// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <utility>

#include "common/logging.h"
#include "video_core/host1x/syncpoint_manager.h"

namespace Tegra {

namespace Host1x {

// The puller's fence action and the 3D sync_info register take the syncpoint id from the guest's
// command stream (24 and 16 bits): an id past the table indexed unrelated memory.
bool SyncpointManager::IsValidSyncpoint(u32 syncpoint_id) {
    if (syncpoint_id < SyncpointManager::NUM_MAX_SYNCPOINTS) {
        return true;
    }
    static std::atomic_flag logged;
    if (!logged.test_and_set()) {
        LOG_ERROR(HW_GPU, "Syncpoint {} is out of range, ignored (logged once)", syncpoint_id);
    }
    return false;
}

SyncpointManager::ActionHandle SyncpointManager::RegisterAction(
    std::atomic<u32>& syncpoint, std::list<RegisteredAction>& action_storage, u32 expected_value,
    std::function<void()>&& action) {
    if (syncpoint.load(std::memory_order_acquire) >= expected_value) {
        action();
        return {};
    }

    std::scoped_lock lk(guard);
    if (syncpoint.load(std::memory_order_relaxed) >= expected_value) {
        action();
        return {};
    }
    auto it = action_storage.begin();
    while (it != action_storage.end()) {
        if (it->expected_value >= expected_value) {
            break;
        }
        ++it;
    }
    return action_storage.emplace(it, expected_value, std::move(action));
}

void SyncpointManager::DeregisterAction(std::list<RegisteredAction>& action_storage,
                                        const ActionHandle& handle) {
    std::scoped_lock lk(guard);

    // We want to ensure the iterator still exists prior to erasing it
    // Otherwise, if an invalid iterator was passed in then it could lead to UB
    // It is important to avoid UB in that case since the deregister isn't called from a locked
    // context
    for (auto it = action_storage.begin(); it != action_storage.end(); it++) {
        if (it == handle) {
            action_storage.erase(it);
            return;
        }
    }
}

void SyncpointManager::DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle) {
    if (!IsValidSyncpoint(syncpoint_id)) {
        return;
    }
    DeregisterAction(guest_action_storage[syncpoint_id], handle);
}

void SyncpointManager::DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle) {
    if (!IsValidSyncpoint(syncpoint_id)) {
        return;
    }
    DeregisterAction(host_action_storage[syncpoint_id], handle);
}

void SyncpointManager::IncrementGuest(u32 syncpoint_id) {
    if (!IsValidSyncpoint(syncpoint_id)) {
        return;
    }
    Increment(syncpoints_guest[syncpoint_id], wait_guest_cv, guest_action_storage[syncpoint_id],
              syncpoint_id);
}

SyncpointManager::GuestMirrorHandle SyncpointManager::RegisterGuestMirror(GuestMirror mirror,
                                                                         bool seed_initial) {
    if (!mirror) {
        return InvalidGuestMirrorHandle;
    }
    std::scoped_lock lk(guard);
    if (next_mirror_handle == InvalidGuestMirrorHandle) {
        ++next_mirror_handle;
    }
    const GuestMirrorHandle handle{next_mirror_handle++};
    if (seed_initial) {
        for (u32 id = 0; id < NUM_MAX_SYNCPOINTS; ++id) {
            mirror(id, syncpoints_guest[id].load(std::memory_order_acquire));
        }
    }
    guest_mirrors.push_back({handle, std::move(mirror), {}});
    return handle;
}

void SyncpointManager::UnregisterGuestMirror(GuestMirrorHandle handle) {
    if (handle == InvalidGuestMirrorHandle) {
        return;
    }
    std::scoped_lock lk(guard);
    std::erase_if(guest_mirrors, [handle](const auto& entry) { return entry.handle == handle; });
}

std::array<u64, SyncpointManager::NUM_MOVED_WORDS> SyncpointManager::TakeMovedGuestForMirror(
    GuestMirrorHandle handle) {
    if (handle == InvalidGuestMirrorHandle) {
        return {};
    }
    std::scoped_lock lk(guard);
    for (auto& entry : guest_mirrors) {
        if (entry.handle == handle) {
            return std::exchange(entry.moved, {});
        }
    }
    return {};
}

void SyncpointManager::IncrementHost(u32 syncpoint_id) {
    if (!IsValidSyncpoint(syncpoint_id)) {
        return;
    }
    Increment(syncpoints_host[syncpoint_id], wait_host_cv, host_action_storage[syncpoint_id]);
}

void SyncpointManager::WaitGuest(u32 syncpoint_id, u32 expected_value) {
    if (!IsValidSyncpoint(syncpoint_id)) {
        return;
    }
    Wait(syncpoints_guest[syncpoint_id], wait_guest_cv, expected_value);
}

void SyncpointManager::WaitHost(u32 syncpoint_id, u32 expected_value) {
    if (!IsValidSyncpoint(syncpoint_id)) {
        return;
    }
    Wait(syncpoints_host[syncpoint_id], wait_host_cv, expected_value);
}

void SyncpointManager::Increment(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                                 std::list<RegisteredAction>& action_storage,
                                 std::optional<u32> mirrored_id) {
    std::scoped_lock lk(guard);
    if (mirrored_id && !guest_mirrors.empty()) {
        // Before the value is published: the lock-free ready checks in RegisterAction and Wait
        // must not see it ahead of the page.
        const u32 next_value{syncpoint.load(std::memory_order_relaxed) + 1};
        const u32 word = *mirrored_id / 64;
        const u64 bit = u64{1} << (*mirrored_id % 64);
        for (auto& entry : guest_mirrors) {
            entry.callback(*mirrored_id, next_value);
            entry.moved[word] |= bit;
        }
    }
    const u32 new_value{syncpoint.fetch_add(1, std::memory_order_acq_rel) + 1};
    if (mirrored_id) {
        // After the mirror and value publish: lock-free TakeMovedGuest readers see the new value.
        guest_moved[*mirrored_id / 64].fetch_or(u64{1} << (*mirrored_id % 64),
                                                std::memory_order_acq_rel);
    }
    auto it = action_storage.begin();
    while (it != action_storage.end()) {
        if (it->expected_value > new_value) {
            break;
        }
        it->action();
        it = action_storage.erase(it);
    }
    wait_cv.notify_all();
}

void SyncpointManager::Wait(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                            u32 expected_value) {
    const auto pred = [&]() { return syncpoint.load(std::memory_order_acquire) >= expected_value; };
    if (pred()) {
        return;
    }

    std::unique_lock lk(guard);
    wait_cv.wait(lk, pred);
}

} // namespace Host1x

} // namespace Tegra
