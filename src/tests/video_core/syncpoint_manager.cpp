// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/scope_exit.h"
#include "core/hle/service/nx2_gsv.h"
#include "video_core/host1x/syncpoint_manager.h"

using Tegra::Host1x::SyncpointManager;

namespace {

struct SimulatedSyncptMemory {
    std::array<u8, 0x1000> rw{};
    std::array<u8, 0x2000> ro{};

    template <size_t N>
    static u64 Read64At(const std::array<u8, N>& page, u64 offset) {
        REQUIRE(offset <= N - sizeof(u64));
        u64 value{};
        std::memcpy(&value, page.data() + offset, sizeof(value));
        return value;
    }

    template <size_t N>
    static void Write64At(std::array<u8, N>& page, u64 offset, u64 value) {
        REQUIRE(offset <= N - sizeof(u64));
        std::memcpy(page.data() + offset, &value, sizeof(value));
    }

    u64 ReadRw64(u64 offset) const {
        return Read64At(rw, offset);
    }

    void WriteRw64(u64 offset, u64 value) {
        Write64At(rw, offset, value);
    }

    u64 ReadRo64(u64 offset) const {
        return Read64At(ro, offset);
    }

    void WriteRo64(u64 offset, u64 value) {
        Write64At(ro, offset, value);
    }

    u64 ReadValue(u32 id) const {
        return ReadRo64(Service::Nx2SyncptMemoryLayout::ValuesOffset + u64{id} * sizeof(u64));
    }

    SyncpointManager::GuestMirror MakeMirror(u32 count) {
        return [this, count](u32 id, u32 value) {
            if (id < count) {
                WriteRo64(Service::Nx2SyncptMemoryLayout::ValuesOffset + u64{id} * sizeof(u64),
                          value);
            }
        };
    }

    bool Poll(SyncpointManager& manager, SyncpointManager::GuestMirrorHandle handle, u32 count,
              u64& last_rw_summary, bool& has_last_rw_summary) {
        return Service::Nx2PollSyncptPages(
            manager, handle, count, last_rw_summary, has_last_rw_summary,
            [this](u64 offset) { return ReadRw64(offset); },
            [this](u64 offset) { return ReadRo64(offset); },
            [this](u64 offset, u64 value) { WriteRo64(offset, value); });
    }
};

} // namespace

TEST_CASE("Syncpoint ids past the table are ignored safely while valid ones work",
          "[video_core][syncpoint]") {
    const auto manager = std::make_unique<SyncpointManager>();
    constexpr u32 Last = static_cast<u32>(SyncpointManager::NUM_MAX_SYNCPOINTS) - 1U;
    constexpr u32 Past = static_cast<u32>(SyncpointManager::NUM_MAX_SYNCPOINTS);

    bool guest = false;
    bool host = false;
    REQUIRE(manager->RegisterGuestAction(Past, 1, [&guest] { guest = true; }) ==
            SyncpointManager::ActionHandle{});
    REQUIRE(guest);
    REQUIRE(manager->RegisterHostAction(Past + 1, 1, [&host] { host = true; }) ==
            SyncpointManager::ActionHandle{});
    REQUIRE(host);

    manager->DeregisterGuestAction(Past, {});
    manager->DeregisterHostAction(Past + 1, {});
    manager->IncrementGuest(Past);
    manager->IncrementHost(0xFFFFFFU);
    manager->WaitGuest(Past, 1);
    manager->WaitHost(Past + 1, 1);
    CHECK(manager->IsReadyGuest(Past, 1));
    CHECK(manager->IsReadyHost(Past + 1, 1));
    CHECK(manager->GetGuestSyncpointValue(Past) == 0U);
    CHECK(manager->GetHostSyncpointValue(Past + 1) == 0U);
    CHECK(manager->TakeMovedGuest(static_cast<u32>(SyncpointManager::NUM_MOVED_WORDS)) == u64{0});
    CHECK(manager->TakeMovedGuestForMirror(SyncpointManager::InvalidGuestMirrorHandle) ==
          std::array<u64, SyncpointManager::NUM_MOVED_WORDS>{});

    bool valid = false;
    const auto guest_handle =
        manager->RegisterGuestAction(Last, 1, [&valid] { valid = true; });
    CHECK_FALSE(valid);
    CHECK_FALSE(manager->IsReadyGuest(Last, 1));
    const auto host_handle =
        manager->RegisterHostAction(Last, 1, [&valid] { valid = true; });
    CHECK_FALSE(manager->IsReadyHost(Last, 1));

    manager->DeregisterGuestAction(Last, guest_handle);
    manager->DeregisterHostAction(Last, host_handle);
    manager->IncrementGuest(Last);
    manager->IncrementHost(Last);
    CHECK_FALSE(valid);
    CHECK(manager->IsReadyGuest(Last, 1));
    CHECK(manager->IsReadyHost(Last, 1));
    CHECK(manager->GetGuestSyncpointValue(Last) == 1U);
    CHECK(manager->GetHostSyncpointValue(Last) == 1U);
}

TEST_CASE("Seeded guest mirror snapshots existing values and moved syncpoints report once",
          "[video_core][syncpoint]") {
    const auto manager = std::make_unique<SyncpointManager>();
    manager->IncrementGuest(70);
    REQUIRE(manager->TakeMovedGuest(1) == (u64{1} << 6));
    REQUIRE(manager->TakeMovedGuest(1) == u64{0});

    u32 unseeded_calls = 0;
    const auto unseeded_handle =
        manager->RegisterGuestMirror([&](u32, u32) { ++unseeded_calls; });
    REQUIRE(unseeded_handle != SyncpointManager::InvalidGuestMirrorHandle);
    CHECK(unseeded_calls == 0U);
    manager->WithGuestValuesStable([&] { CHECK(manager->GetGuestSyncpointValue(70) == 1U); });

    std::vector<u32> page(SyncpointManager::NUM_MAX_SYNCPOINTS, 0xFFFFU);
    const auto seeded_handle = manager->RegisterGuestMirror(
        [&page](u32 id, u32 value) { page[id] = value; }, true);
    REQUIRE(seeded_handle != SyncpointManager::InvalidGuestMirrorHandle);
    CHECK(page[70] == 1U);
    CHECK(page[0] == 0U);
    CHECK(page[SyncpointManager::NUM_MAX_SYNCPOINTS - 1] == 0U);
    // Pre-registration increments were already covered by the initial snapshot.
    CHECK(manager->TakeMovedGuestForMirror(seeded_handle) ==
          std::array<u64, SyncpointManager::NUM_MOVED_WORDS>{});

    u32 seen_by_action = 0;
    manager->RegisterGuestAction(70, 2, [&] { seen_by_action = page[70]; });
    manager->IncrementGuest(70);
    manager->IncrementGuest(191);
    manager->IncrementHost(3);

    CHECK(seen_by_action == 2U);
    CHECK(page[70] == 2U);
    CHECK(page[191] == 1U);
    CHECK(unseeded_calls == 2U);
    CHECK(manager->TakeMovedGuest(0) == u64{0});
    CHECK(manager->TakeMovedGuest(1) == (u64{1} << 6));
    CHECK(manager->TakeMovedGuest(2) == (u64{1} << 63));
    CHECK(manager->TakeMovedGuest(2) == u64{0});

    const auto moved = manager->TakeMovedGuestForMirror(seeded_handle);
    CHECK(moved[0] == u64{0});
    CHECK(moved[1] == (u64{1} << 6));
    CHECK(moved[2] == (u64{1} << 63));
    CHECK(manager->TakeMovedGuestForMirror(seeded_handle) ==
          std::array<u64, SyncpointManager::NUM_MOVED_WORDS>{});

    manager->UnregisterGuestMirror(seeded_handle);
    manager->UnregisterGuestMirror(unseeded_handle);
    manager->IncrementGuest(70);
    CHECK(page[70] == 2U);
}

TEST_CASE("Multiple and re-registered mirrors track moved syncpoints independently",
          "[video_core][syncpoint]") {
    const auto manager = std::make_unique<SyncpointManager>();
    manager->IncrementGuest(5);

    std::vector<u32> page_a(SyncpointManager::NUM_MAX_SYNCPOINTS, 0xFFFFU);
    std::vector<u32> page_b(SyncpointManager::NUM_MAX_SYNCPOINTS, 0xFFFFU);
    const auto handle_a =
        manager->RegisterGuestMirror([&](u32 id, u32 val) { page_a[id] = val; }, true);
    const auto handle_b =
        manager->RegisterGuestMirror([&](u32 id, u32 val) { page_b[id] = val; }, true);

    CHECK(page_a[5] == 1U);
    CHECK(page_b[5] == 1U);

    manager->IncrementGuest(5);
    manager->IncrementGuest(130);

    const auto moved_a = manager->TakeMovedGuestForMirror(handle_a);
    CHECK(moved_a[0] == (u64{1} << 5));
    CHECK(moved_a[2] == (u64{1} << 2));
    CHECK(manager->TakeMovedGuestForMirror(handle_a) ==
          std::array<u64, SyncpointManager::NUM_MOVED_WORDS>{});

    // Consuming handle_a must not wipe moved state for handle_b.
    const auto moved_b = manager->TakeMovedGuestForMirror(handle_b);
    CHECK(moved_b[0] == (u64{1} << 5));
    CHECK(moved_b[2] == (u64{1} << 2));

    // Re-registering A before unregistering the old mirror keeps snapshots and moves coherent.
    std::vector<u32> page_a2(SyncpointManager::NUM_MAX_SYNCPOINTS, 0xFFFFU);
    const auto handle_a2 =
        manager->RegisterGuestMirror([&](u32 id, u32 val) { page_a2[id] = val; }, true);
    CHECK(page_a2[5] == 2U);
    CHECK(page_a2[130] == 1U);

    manager->IncrementGuest(5);
    manager->UnregisterGuestMirror(handle_a);
    CHECK(manager->TakeMovedGuestForMirror(handle_a) ==
          std::array<u64, SyncpointManager::NUM_MOVED_WORDS>{});

    const auto moved_a2 = manager->TakeMovedGuestForMirror(handle_a2);
    CHECK(moved_a2[0] == (u64{1} << 5));
    CHECK(moved_a2[2] == u64{0});
    CHECK(page_a2[5] == 3U);
    CHECK(page_b[5] == 3U);

    manager->UnregisterGuestMirror(handle_a2);
    manager->UnregisterGuestMirror(handle_b);
}

TEST_CASE("Nx2PollSyncptPages wakes already-armed syncpoints across re-arms and sessions",
          "[video_core][syncpoint]") {
    const auto manager = std::make_unique<SyncpointManager>();
    manager->IncrementGuest(70);

    SimulatedSyncptMemory mem_a;
    SimulatedSyncptMemory mem_b;
    constexpr u32 Count = 192;
    const auto handle_a = manager->RegisterGuestMirror(mem_a.MakeMirror(Count), true);
    const auto handle_b = manager->RegisterGuestMirror(mem_b.MakeMirror(Count), true);

    CHECK(mem_a.ReadValue(70) == u64{1});
    CHECK(mem_b.ReadValue(70) == u64{1});

    u64 summary_a{};
    bool has_summary_a = false;
    u64 summary_b{};
    bool has_summary_b = false;

    // Guest arms bit 6 in word 1 (syncpoint 70) in both sessions.
    mem_a.WriteRw64(Service::Nx2SyncptMemoryLayout::RwBitsOffset + u64{8}, u64{1} << 6);
    mem_a.WriteRw64(Service::Nx2SyncptMemoryLayout::RwSummaryOffset, u64{1} << 1);
    mem_b.WriteRw64(Service::Nx2SyncptMemoryLayout::RwBitsOffset + u64{8}, u64{1} << 6);
    mem_b.WriteRw64(Service::Nx2SyncptMemoryLayout::RwSummaryOffset, u64{1} << 1);

    REQUIRE(mem_a.Poll(*manager, handle_a, Count, summary_a, has_summary_a));
    REQUIRE(mem_b.Poll(*manager, handle_b, Count, summary_b, has_summary_b));
    CHECK(mem_a.ReadRo64(Service::Nx2SyncptMemoryLayout::RoBitsOffset + u64{8}) == (u64{1} << 6));
    CHECK(mem_a.ReadRo64(Service::Nx2SyncptMemoryLayout::RoSummaryOffset) == (u64{1} << 1));

    // Idle poll with no change and no moved syncpoint does not wake.
    CHECK_FALSE(mem_a.Poll(*manager, handle_a, Count, summary_a, has_summary_a));
    CHECK_FALSE(mem_b.Poll(*manager, handle_b, Count, summary_b, has_summary_b));

    // Client keeps the same bit armed (rw_bits == ro_bits) and waits for syncpoint 70 to move.
    manager->IncrementGuest(70);
    CHECK(mem_a.ReadValue(70) == u64{2});
    CHECK(mem_b.ReadValue(70) == u64{2});
    CHECK(mem_a.Poll(*manager, handle_a, Count, summary_a, has_summary_a));
    CHECK(mem_b.Poll(*manager, handle_b, Count, summary_b, has_summary_b));

    // Moved bits are consumed once.
    CHECK_FALSE(mem_a.Poll(*manager, handle_a, Count, summary_a, has_summary_a));
    CHECK_FALSE(mem_b.Poll(*manager, handle_b, Count, summary_b, has_summary_b));

    // Unarmed syncpoint increment drains moved state without spurious wakeup.
    manager->IncrementGuest(71);
    CHECK_FALSE(mem_a.Poll(*manager, handle_a, Count, summary_a, has_summary_a));

    // Clamping handles count == 0 and oversized counts without overflowing.
    CHECK_FALSE(mem_a.Poll(*manager, handle_a, 0, summary_a, has_summary_a));
    CHECK_FALSE(mem_a.Poll(*manager, handle_a, std::numeric_limits<u32>::max(), summary_a,
                           has_summary_a));

    manager->UnregisterGuestMirror(handle_a);
    manager->UnregisterGuestMirror(handle_b);
}

TEST_CASE("Seeded mirror registration and WithGuestValuesStable stay monotonic under contention",
          "[video_core][syncpoint]") {
    const auto manager = std::make_unique<SyncpointManager>();
    constexpr u32 Syncpt = 12;
    constexpr u32 Iterations = 2000;

    std::atomic<bool> start{false};
    std::jthread incrementer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (u32 i = 0; i < Iterations; ++i) {
            manager->IncrementGuest(Syncpt);
        }
    });

    start.store(true, std::memory_order_release);
    for (u32 round = 0; round < 32U; ++round) {
        std::atomic<u32> mirrored_value{0};
        std::atomic<bool> monotonic{true};
        u32 seeded_value = 0;
        bool seeded = false;
        const auto handle = manager->RegisterGuestMirror(
            [&](u32 id, u32 value) {
                if (id == Syncpt) {
                    if (!seeded) {
                        seeded = true;
                        seeded_value = value;
                    }
                    const u32 prev = mirrored_value.exchange(value, std::memory_order_acq_rel);
                    if (value < prev) {
                        monotonic.store(false, std::memory_order_release);
                    }
                }
            },
            true);
        SCOPE_EXIT {
            manager->UnregisterGuestMirror(handle);
        };
        REQUIRE(handle != SyncpointManager::InvalidGuestMirrorHandle);
        REQUIRE(seeded);

        u32 stable_value = 0;
        manager->WithGuestValuesStable([&] {
            stable_value = manager->GetGuestSyncpointValue(Syncpt);
            REQUIRE(mirrored_value.load(std::memory_order_acquire) == stable_value);
        });

        std::this_thread::yield();
        const auto moved = manager->TakeMovedGuestForMirror(handle);
        if ((moved[0] & (u64{1} << Syncpt)) != u64{0}) {
            const u32 observed = mirrored_value.load(std::memory_order_acquire);
            REQUIRE(observed > seeded_value);
            REQUIRE(observed >= stable_value);
        }
        REQUIRE(monotonic.load(std::memory_order_acquire));
    }

    incrementer.join();
    CHECK(manager->GetGuestSyncpointValue(Syncpt) == Iterations);
}
