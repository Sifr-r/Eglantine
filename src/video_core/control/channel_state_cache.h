// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include "common/container/unordered_map.h"
#include <vector>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/control/channel_state_bridge.h"

namespace Tegra {

namespace Engines {
class Maxwell3D;
class KeplerCompute;
} // namespace Engines

class MemoryManager;

namespace Control {
struct ChannelState;
}

} // namespace Tegra

namespace VideoCommon {

class ChannelInfo {
public:
    ChannelInfo() = delete;
    explicit ChannelInfo(Tegra::Control::ChannelState& state);
    explicit ChannelInfo(const Tegra::Control::ChannelStateBridge& bridge_);
    ChannelInfo(const ChannelInfo& state) = delete;
    ChannelInfo& operator=(const ChannelInfo&) = delete;

    [[nodiscard]] const Tegra::Control::ChannelStateBridge& Bridge() const noexcept {
        return bridge;
    }

    [[nodiscard]] Tegra::Control::ChannelStateBridge& Bridge() noexcept {
        return bridge;
    }

    Tegra::Control::ChannelStateBridge bridge;
    Tegra::Engines::Maxwell3D& maxwell3d;
    Tegra::Engines::KeplerCompute& kepler_compute;
    Tegra::MemoryManager& gpu_memory;
    u64 program_id;
};

template <class P>
class ChannelSetupCaches {
public:
    /// Operations for setting the channel of execution.
    virtual ~ChannelSetupCaches();

    /// Create channel state.
    virtual void CreateChannel(Tegra::Control::ChannelState& channel);

    /// Create channel state from a ChannelStateBridge without requiring a raw ChannelState.
    virtual void CreateChannelFromBridge(const Tegra::Control::ChannelStateBridge& bridge);

    /// Bind a channel for execution.
    virtual void BindToChannel(s32 id);

    /// Erase channel's state.
    void EraseChannel(s32 id);

    [[nodiscard]] Tegra::Control::ChannelStateBridge CurrentBridge() const noexcept {
        if constexpr (std::is_base_of_v<ChannelInfo, P>) {
            if (channel_state != nullptr && channel_state->Bridge().IsBound()) {
                return channel_state->Bridge();
            }
        }
        if (maxwell3d != nullptr && kepler_compute != nullptr && gpu_memory != nullptr) {
            return Tegra::Control::ChannelStateBridge{*maxwell3d, *kepler_compute, *gpu_memory,
                                                      program_id, current_bind_id};
        }
        return {};
    }

    [[nodiscard]] std::optional<Tegra::Control::ChannelStateBridge> FindBridge(s32 id) const {
        std::unique_lock<std::mutex> lk(config_mutex);
        const auto pair = channel_map.find(id);
        if (pair == channel_map.end() || pair->second >= channel_storage.size() ||
            !channel_storage[pair->second].has_value()) {
            return std::nullopt;
        }
        const auto& info = *channel_storage[pair->second];
        if constexpr (std::is_base_of_v<ChannelInfo, P>) {
            return info.Bridge();
        } else {
            return Tegra::Control::ChannelStateBridge{info.maxwell3d, info.kepler_compute,
                                                      info.gpu_memory, info.program_id, id};
        }
    }

    Tegra::MemoryManager* GetFromID(size_t id) const {
        std::unique_lock<std::mutex> lk(config_mutex);
        const auto ref = address_spaces.find(id);
        if (ref == address_spaces.end()) {
            return nullptr;
        }
        return ref->second.gpu_memory;
    }

    std::optional<size_t> getStorageID(size_t id) const {
        std::unique_lock<std::mutex> lk(config_mutex);
        const auto ref = address_spaces.find(id);
        if (ref == address_spaces.end()) {
            return std::nullopt;
        }
        return ref->second.storage_id;
    }

protected:
    static constexpr size_t UNSET_CHANNEL{(std::numeric_limits<size_t>::max)()};

    P* channel_state = nullptr;
    size_t current_channel_id{UNSET_CHANNEL};
    s32 current_bind_id{Tegra::Control::ChannelStateBridge::UnboundChannelId};
    size_t current_address_space{};
    Tegra::Engines::Maxwell3D* maxwell3d{};
    Tegra::Engines::KeplerCompute* kepler_compute{};
    Tegra::MemoryManager* gpu_memory{};
    u64 program_id{};

    std::deque<std::optional<P>> channel_storage;
    std::deque<size_t> free_channel_ids;
    ::Common::unordered_map<s32, size_t> channel_map;
    std::vector<size_t> active_channel_ids;
    struct AddressSpaceRef {
        size_t ref_count;
        size_t storage_id;
        Tegra::MemoryManager* gpu_memory;
        std::weak_ptr<Tegra::MemoryManager> owner;
    };
    ::Common::unordered_map<size_t, AddressSpaceRef> address_spaces;
    mutable std::mutex config_mutex;

    virtual void OnGPUASRegister([[maybe_unused]] size_t map_id) {}
};

template <class P>
void ChannelSetupCaches<P>::CreateChannelFromBridge(
    const Tegra::Control::ChannelStateBridge& bridge) {
    if (!bridge.HasChannelBindings() || bridge.BindId() < 0) {
        LOG_ERROR(HW_GPU,
                  "Invalid ChannelStateBridge for channel creation (bind_id={}, bindings={})",
                  bridge.BindId(), bridge.HasChannelBindings());
        return;
    }
    {
        std::unique_lock<std::mutex> lk(config_mutex);
        if (channel_map.find(bridge.BindId()) != channel_map.end()) {
            LOG_ERROR(HW_GPU, "Duplicate channel {} in CreateChannelFromBridge", bridge.BindId());
            return;
        }
    }
    if (bridge.HasChannelState()) {
        CreateChannel(*bridge.RawChannelState());
        return;
    }
    if constexpr (std::is_constructible_v<P, const Tegra::Control::ChannelStateBridge&>) {
        std::unique_lock<std::mutex> lk(config_mutex);
        if (channel_map.find(bridge.BindId()) != channel_map.end()) {
            LOG_ERROR(HW_GPU, "Duplicate channel {} in CreateChannelFromBridge", bridge.BindId());
            return;
        }
        const size_t new_id = [this, &bridge]() {
            if (!free_channel_ids.empty()) {
                const auto slot_id = free_channel_ids.front();
                free_channel_ids.pop_front();
                channel_storage[slot_id].emplace(bridge);
                return slot_id;
            }
            channel_storage.emplace_back(std::in_place, bridge);
            return channel_storage.size() - 1;
        }();
        channel_map.emplace(bridge.BindId(), new_id);
        if (current_channel_id != UNSET_CHANNEL) {
            channel_state = &*channel_storage[current_channel_id];
        }
        active_channel_ids.push_back(new_id);
        auto* const memory_manager = bridge.GpuMemoryPtr();
        ASSERT(memory_manager != nullptr);
        const size_t gpu_as_id = memory_manager->GetID();
        const auto as_it = address_spaces.find(gpu_as_id);
        if (as_it != address_spaces.end()) {
            as_it->second.ref_count++;
            return;
        }
        const AddressSpaceRef new_gpu_mem_ref{
            .ref_count = 1,
            .storage_id = address_spaces.size(),
            .gpu_memory = memory_manager,
            .owner = memory_manager->weak_from_this(),
        };
        address_spaces.emplace(gpu_as_id, new_gpu_mem_ref);
        OnGPUASRegister(gpu_as_id);
    }
}

} // namespace VideoCommon
