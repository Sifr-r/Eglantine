// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <mutex>
#include <unordered_map>

#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/nx2_session_state.h"

namespace Core {

namespace {

struct HandleRegistry {
    static constexpr size_t FastSlotCount = 64;
    static constexpr Nx2SessionState::Handle FastSlotBit = 1ULL;
    static constexpr size_t FastSlotShift = 1;
    static constexpr Nx2SessionState::Handle FastSlotMask = (FastSlotCount - 1) << FastSlotShift;
    static constexpr size_t GenerationShift = 7;
    static constexpr u64 FastGenerationMask = (~u64{0}) >> GenerationShift;

    struct FastSlot {
        std::mutex mutex;
        Nx2SessionState::Handle active_handle{0};
        std::weak_ptr<Nx2SessionState> weak;
    };

    std::atomic<u64> next_generation{1};
    std::atomic<size_t> active_sessions{0};
    std::array<FastSlot, FastSlotCount> fast_slots{};
    std::mutex overflow_mutex;
    u64 next_overflow_generation{1};
    std::unordered_map<Nx2SessionState::Handle, std::weak_ptr<Nx2SessionState>> overflow_slots;
};

HandleRegistry& GetHandleRegistry() {
    static HandleRegistry* const registry = new HandleRegistry();
    return *registry;
}

Nx2SessionState::Handle RegisterSessionHandle(const std::shared_ptr<Nx2SessionState>& state) {
    auto& registry = GetHandleRegistry();

    u64 generation =
        registry.next_generation.fetch_add(1, std::memory_order_relaxed) &
        HandleRegistry::FastGenerationMask;
    if (generation == 0) {
        generation =
            registry.next_generation.fetch_add(1, std::memory_order_relaxed) &
            HandleRegistry::FastGenerationMask;
        if (generation == 0) {
            generation = 1;
        }
    }

    const size_t start = static_cast<size_t>(generation % HandleRegistry::FastSlotCount);
    for (size_t offset = 0; offset < HandleRegistry::FastSlotCount; ++offset) {
        const size_t index = (start + offset) % HandleRegistry::FastSlotCount;
        auto& slot = registry.fast_slots[index];
        std::scoped_lock lk{slot.mutex};
        if (slot.active_handle != 0 && !slot.weak.expired()) {
            continue;
        }
        u64 slot_gen = generation;
        auto candidate = static_cast<Nx2SessionState::Handle>(
            (slot_gen << HandleRegistry::GenerationShift) |
            (static_cast<u64>(index) << HandleRegistry::FastSlotShift) |
            HandleRegistry::FastSlotBit);
        if (candidate == slot.active_handle) {
            slot_gen = (slot_gen + 1) & HandleRegistry::FastGenerationMask;
            if (slot_gen == 0) {
                slot_gen = 1;
            }
            candidate = static_cast<Nx2SessionState::Handle>(
                (slot_gen << HandleRegistry::GenerationShift) |
                (static_cast<u64>(index) << HandleRegistry::FastSlotShift) |
                HandleRegistry::FastSlotBit);
        }
        slot.active_handle = candidate;
        slot.weak = state;
        return candidate;
    }

    std::scoped_lock lk{registry.overflow_mutex};
    std::erase_if(registry.overflow_slots,
                  [](const auto& entry) { return entry.second.expired(); });
    for (;;) {
        const u64 overflow_gen = registry.next_overflow_generation++;
        const Nx2SessionState::Handle overflow_handle = overflow_gen << 1;
        if (overflow_handle == 0) {
            continue;
        }
        if (registry.overflow_slots.find(overflow_handle) == registry.overflow_slots.end()) {
            registry.overflow_slots.emplace(overflow_handle, state);
            return overflow_handle;
        }
    }
}

void UnregisterSessionHandle(Nx2SessionState::Handle handle) {
    if (handle == 0) {
        return;
    }
    auto& registry = GetHandleRegistry();

    if ((handle & HandleRegistry::FastSlotBit) != 0) {
        const size_t index =
            static_cast<size_t>((handle & HandleRegistry::FastSlotMask) >> HandleRegistry::FastSlotShift);
        auto& slot = registry.fast_slots[index];
        std::scoped_lock lk{slot.mutex};
        if (slot.active_handle == handle) {
            slot.active_handle = 0;
            slot.weak.reset();
        }
        return;
    }

    std::scoped_lock lk{registry.overflow_mutex};
    registry.overflow_slots.erase(handle);
}

} // namespace

Nx2SessionState::Nx2SessionState(PrivateTag)
    : gpu_memory_registry{std::make_unique<Service::Nvidia::Devices::Nx2GpuMemoryRegistry>()} {}

Nx2SessionState::~Nx2SessionState() {
    Retire();
}

std::shared_ptr<Nx2SessionState> Nx2SessionState::Create() {
    auto state = std::make_shared<Nx2SessionState>(PrivateTag{});
    const Handle registered = RegisterSessionHandle(state);
    state->handle.store(registered, std::memory_order_release);
    GetHandleRegistry().active_sessions.fetch_add(1, std::memory_order_acq_rel);
    return state;
}

bool Nx2SessionState::HasActiveSessions() noexcept {
    return GetHandleRegistry().active_sessions.load(std::memory_order_acquire) > 0;
}

std::shared_ptr<Nx2SessionState> Nx2SessionState::Lock(Handle handle) {
    if (handle == 0) {
        return {};
    }
    auto& registry = GetHandleRegistry();

    if ((handle & HandleRegistry::FastSlotBit) != 0) {
        const size_t index =
            static_cast<size_t>((handle & HandleRegistry::FastSlotMask) >> HandleRegistry::FastSlotShift);
        auto& slot = registry.fast_slots[index];
        std::scoped_lock lk{slot.mutex};
        if (slot.active_handle != handle) {
            return {};
        }
        auto locked = slot.weak.lock();
        if (!locked || locked->IsRetired()) {
            slot.active_handle = 0;
            slot.weak.reset();
            return {};
        }
        return locked;
    }

    std::scoped_lock lk{registry.overflow_mutex};
    const auto it = registry.overflow_slots.find(handle);
    if (it == registry.overflow_slots.end()) {
        return {};
    }
    auto locked = it->second.lock();
    if (!locked || locked->IsRetired()) {
        registry.overflow_slots.erase(it);
        return {};
    }
    return locked;
}

void Nx2SessionState::Retire() {
    const bool already_retired = retired.exchange(true, std::memory_order_acq_rel);
    const Handle detached = handle.exchange(0, std::memory_order_acq_rel);
    if (detached != 0) {
        UnregisterSessionHandle(detached);
    }
    if (already_retired) {
        return;
    }
    GetHandleRegistry().active_sessions.fetch_sub(1, std::memory_order_acq_rel);
    sparse_state.Clear();
    semaphore_waiters.Clear();
    async_reports.Clear();
    semaphore_regions.Clear();
    if (gpu_memory_registry) {
        gpu_memory_registry->Reset();
    }
    driver_active.store(false, std::memory_order_release);
    ampere.store(false, std::memory_order_release);
    present_count.store(0, std::memory_order_release);
    video_last_frame_ns.store(0, std::memory_order_release);
}

Service::Nvidia::Devices::Nx2GpuMemoryRegistry& Nx2SessionState::GpuMemoryRegistry() noexcept {
    return *gpu_memory_registry;
}

const Service::Nvidia::Devices::Nx2GpuMemoryRegistry& Nx2SessionState::GpuMemoryRegistry()
    const noexcept {
    return *gpu_memory_registry;
}

} // namespace Core
