// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <memory>

#include "common/common_types.h"
#include "video_core/nx2.h"

namespace Service::Nvidia::Devices {
class Nx2GpuMemoryRegistry;
} // namespace Service::Nvidia::Devices

namespace Core {

/// Owns per-session Switch 2 state so consecutive sessions remain isolated and Switch 1 sessions
/// carry no active Switch 2 globals.
class Nx2SessionState final {
    struct PrivateTag {};

public:
    using Handle = u64;

    explicit Nx2SessionState(PrivateTag);
    ~Nx2SessionState();

    Nx2SessionState(const Nx2SessionState&) = delete;
    Nx2SessionState& operator=(const Nx2SessionState&) = delete;
    Nx2SessionState(Nx2SessionState&&) = delete;
    Nx2SessionState& operator=(Nx2SessionState&&) = delete;

    /// Allocates a fresh Switch 2 session state and assigns a compact handle for deferred work.
    [[nodiscard]] static std::shared_ptr<Nx2SessionState> Create();

    /// Locks the session for `handle`, returning nullptr if handle is 0, retired, or destroyed.
    [[nodiscard]] static std::shared_ptr<Nx2SessionState> Lock(Handle handle);

    /// Returns the compact handle for this session, or 0 once retired.
    [[nodiscard]] Handle GetHandle() const noexcept {
        return handle.load(std::memory_order_acquire);
    }

    /// Returns true once Retire() has detached the handle.
    [[nodiscard]] bool IsRetired() const noexcept {
        return retired.load(std::memory_order_acquire) ||
               handle.load(std::memory_order_acquire) == 0;
    }

    /// Detaches the handle and drains sparse backing callbacks before session teardown.
    void Retire();

    [[nodiscard]] static bool HasActiveSessions() noexcept;

    [[nodiscard]] bool IsDriverActive() const noexcept {
        return driver_active.load(std::memory_order_acquire);
    }
    void SetDriverActive(bool active) noexcept {
        driver_active.store(active, std::memory_order_release);
    }

    [[nodiscard]] bool IsAmpere() const noexcept {
        return ampere.load(std::memory_order_acquire);
    }
    void SetAmpere(bool active) noexcept {
        ampere.store(active, std::memory_order_release);
    }

    [[nodiscard]] u64 PresentCount() const noexcept {
        return present_count.load(std::memory_order_acquire);
    }
    u64 IncrementPresentCount() noexcept {
        return present_count.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    [[nodiscard]] s64 VideoLastFrameNs() const noexcept {
        return video_last_frame_ns.load(std::memory_order_acquire);
    }
    void SetVideoLastFrameNs(s64 timestamp_ns) noexcept {
        video_last_frame_ns.store(timestamp_ns, std::memory_order_release);
    }

    [[nodiscard]] Tegra::Nx2SemaphoreWaiters& SemaphoreWaiters() noexcept {
        return semaphore_waiters;
    }
    [[nodiscard]] const Tegra::Nx2SemaphoreWaiters& SemaphoreWaiters() const noexcept {
        return semaphore_waiters;
    }

    [[nodiscard]] Tegra::Nx2AsyncReports& AsyncReports() noexcept {
        return async_reports;
    }
    [[nodiscard]] const Tegra::Nx2AsyncReports& AsyncReports() const noexcept {
        return async_reports;
    }

    void IssueAsyncReport(u16 slot) noexcept {
        async_reports.Issue(slot);
    }

    void CompleteAsyncReport(u16 slot) noexcept {
        async_reports.Complete(slot);
    }

    [[nodiscard]] bool AreAsyncReportsSettled(u64 device_address, u64 size) const noexcept {
        return async_reports.Settled(device_address, size);
    }

    [[nodiscard]] Tegra::Nx2SemaphoreRegions& SemaphoreRegions() noexcept {
        return semaphore_regions;
    }
    [[nodiscard]] const Tegra::Nx2SemaphoreRegions& SemaphoreRegions() const noexcept {
        return semaphore_regions;
    }

    [[nodiscard]] Tegra::Nx2SparseState& SparseState() noexcept {
        return sparse_state;
    }
    [[nodiscard]] const Tegra::Nx2SparseState& SparseState() const noexcept {
        return sparse_state;
    }

    [[nodiscard]] Service::Nvidia::Devices::Nx2GpuMemoryRegistry& GpuMemoryRegistry() noexcept;
    [[nodiscard]] const Service::Nvidia::Devices::Nx2GpuMemoryRegistry& GpuMemoryRegistry()
        const noexcept;

private:
    std::atomic<Handle> handle{0};
    Tegra::Nx2SemaphoreWaiters semaphore_waiters;
    Tegra::Nx2AsyncReports async_reports;
    Tegra::Nx2SemaphoreRegions semaphore_regions;
    Tegra::Nx2SparseState sparse_state;
    std::unique_ptr<Service::Nvidia::Devices::Nx2GpuMemoryRegistry> gpu_memory_registry;
    std::atomic<bool> retired{false};
    std::atomic<bool> driver_active{false};
    std::atomic<bool> ampere{false};
    std::atomic<u64> present_count{0};
    std::atomic<s64> video_last_frame_ns{0};
};

} // namespace Core
