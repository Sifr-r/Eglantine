// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <list>
#include <optional>
#include <mutex>
#include <vector>

#include "common/common_types.h"

namespace Tegra {

namespace Host1x {

class SyncpointManager {
public:
    static constexpr size_t NUM_MAX_SYNCPOINTS = 192;
    static constexpr size_t NUM_MOVED_WORDS = (NUM_MAX_SYNCPOINTS + 63) / 64;

    /// False (logged once) for an id past the tables; such ids come from guest data.
    static bool IsValidSyncpoint(u32 syncpoint_id);

    u32 GetGuestSyncpointValue(u32 id) const {
        if (!IsValidSyncpoint(id)) {
            return 0;
        }
        return syncpoints_guest[id].load(std::memory_order_acquire);
    }

    u32 GetHostSyncpointValue(u32 id) const {
        if (!IsValidSyncpoint(id)) {
            return 0;
        }
        return syncpoints_host[id].load(std::memory_order_acquire);
    }

    struct RegisteredAction {
        explicit RegisteredAction(u32 expected_value_, std::function<void()>&& action_)
            : expected_value{expected_value_}, action{std::move(action_)} {}
        u32 expected_value;
        std::function<void()> action;
    };
    using ActionHandle = std::list<RegisteredAction>::iterator;

    template <typename Func>
    ActionHandle RegisterGuestAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
        if (!IsValidSyncpoint(syncpoint_id)) {
            // No such syncpoint to wait on: count it as reached, or a composite never runs.
            action();
            return {};
        }
        return RegisterAction(syncpoints_guest[syncpoint_id], guest_action_storage[syncpoint_id],
                              expected_value, std::move(action));
    }

    template <typename Func>
    ActionHandle RegisterHostAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
        if (!IsValidSyncpoint(syncpoint_id)) {
            action();
            return {};
        }
        return RegisterAction(syncpoints_host[syncpoint_id], host_action_storage[syncpoint_id],
                              expected_value, std::move(action));
    }

    void DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle);

    void DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle);

    void IncrementGuest(u32 syncpoint_id);

    /// Called with a syncpoint's new guest value on every guest increment, before the value is
    /// published and the actions it meets run. Switch 2 clients read syncpoint values from a page
    /// the gsv service fills, and an action may wake a client that reads it right away.
    /// When `seed_initial` is true, invokes `mirror` for every syncpoint under the guard upon
    /// registration so the page starts with an atomic snapshot.
    using GuestMirror = std::function<void(u32 syncpoint_id, u32 value)>;
    using GuestMirrorHandle = u64;
    static constexpr GuestMirrorHandle InvalidGuestMirrorHandle = 0;

    [[nodiscard]] GuestMirrorHandle RegisterGuestMirror(GuestMirror mirror,
                                                        bool seed_initial = false);
    void UnregisterGuestMirror(GuestMirrorHandle handle);

    /// Bits of the guest syncpoints 64 * word .. 64 * word + 63 incremented since the last call.
    u64 TakeMovedGuest(u32 word) {
        if (word >= guest_moved.size()) {
            return 0;
        }
        return guest_moved[word].exchange(0, std::memory_order_acq_rel);
    }

    /// Bits of the guest syncpoints incremented for `handle` since registration or the last call.
    std::array<u64, NUM_MOVED_WORDS> TakeMovedGuestForMirror(GuestMirrorHandle handle);

    /// Runs func with guest increments held off, so a copy of the guest values it makes can't
    /// overwrite a newer value the mirror wrote.
    template <typename Func>
    void WithGuestValuesStable(Func&& func) {
        std::scoped_lock lk(guard);
        func();
    }

    void IncrementHost(u32 syncpoint_id);

    void WaitGuest(u32 syncpoint_id, u32 expected_value);

    void WaitHost(u32 syncpoint_id, u32 expected_value);

    bool IsReadyGuest(u32 syncpoint_id, u32 expected_value) const {
        return !IsValidSyncpoint(syncpoint_id) ||
               syncpoints_guest[syncpoint_id].load(std::memory_order_acquire) >= expected_value;
    }

    bool IsReadyHost(u32 syncpoint_id, u32 expected_value) const {
        return !IsValidSyncpoint(syncpoint_id) ||
               syncpoints_host[syncpoint_id].load(std::memory_order_acquire) >= expected_value;
    }

private:
    void Increment(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                   std::list<RegisteredAction>& action_storage,
                   std::optional<u32> mirrored_id = std::nullopt);

    ActionHandle RegisterAction(std::atomic<u32>& syncpoint,
                                std::list<RegisteredAction>& action_storage, u32 expected_value,
                                std::function<void()>&& action);

    void DeregisterAction(std::list<RegisteredAction>& action_storage, const ActionHandle& handle);

    void Wait(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv, u32 expected_value);

    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> syncpoints_guest{};
    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> syncpoints_host{};

    std::array<std::list<RegisteredAction>, NUM_MAX_SYNCPOINTS> guest_action_storage;
    std::array<std::list<RegisteredAction>, NUM_MAX_SYNCPOINTS> host_action_storage;

    struct GuestMirrorEntry {
        GuestMirrorHandle handle{};
        GuestMirror callback;
        std::array<u64, NUM_MOVED_WORDS> moved{};
    };

    std::mutex guard;
    GuestMirrorHandle next_mirror_handle{1};   ///< Guarded by `guard`.
    std::vector<GuestMirrorEntry> guest_mirrors; ///< Guarded by `guard`.
    std::array<std::atomic<u64>, NUM_MOVED_WORDS> guest_moved{};
    std::condition_variable wait_guest_cv;
    std::condition_variable wait_host_cv;
};

} // namespace Host1x

} // namespace Tegra
