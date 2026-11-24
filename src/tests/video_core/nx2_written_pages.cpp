// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <barrier>
#include <limits>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2.h"
#include "video_core/nx2_gpu_latency.h"

namespace {

constexpr u64 PageSize = u64{1} << Tegra::Nx2WrittenPageBits;
constexpr u64 AddressLimit = u64{1} << 36;

struct WrittenPages {
    WrittenPages() {
        Tegra::Nx2ClearGpuWrittenPages();
    }
    ~WrittenPages() {
        Tegra::Nx2ClearGpuWrittenPages();
    }

    void MarkReference(u64 address, u64 size) {
        if (size == 0) {
            return;
        }
        const u64 last = std::min<u64>((address + size - 1) >> Tegra::Nx2WrittenPageBits,
                                       expected.size() * 64 - 1);
        for (u64 page = address >> Tegra::Nx2WrittenPageBits; page <= last; ++page) {
            expected[page / 64] |= u64{1} << (page % 64);
        }
    }

    void Check() const {
        for (size_t i = 0; i < expected.size(); ++i) {
            if (Tegra::g_nx2_gpu_written_pages[i].load(std::memory_order_relaxed) != expected[i]) {
                FAIL("GPU-written bitmap mismatch at word " << i);
            }
        }
        SUCCEED("GPU-written bitmap matches every reference word");
    }

    std::vector<u64> expected = std::vector<u64>(Tegra::g_nx2_gpu_written_pages.size());
};

} // namespace

TEST_CASE("GPU-written page marking preserves boundaries and wrapping arithmetic",
          "[video_core][nx2_written_pages]") {
    constexpr u64 Max = std::numeric_limits<u64>::max();
    const std::pair<u64, u64> ranges[]{
        {0, 0},
        {0, 1},
        {1, 1},
        {PageSize - 1, 2},
        {0, PageSize},
        {0, 63 * PageSize},
        {0, 64 * PageSize},
        {0, 65 * PageSize},
        {63 * PageSize + 1, PageSize},
        {64 * PageSize - 1, 2},
        {61 * PageSize + 7, 130 * PageSize},
        {AddressLimit - 1, 1},
        {AddressLimit - PageSize, PageSize * 3},
        {AddressLimit, 1},
        {AddressLimit + PageSize, PageSize},
        {Max, 0},
        {Max, 1},
        {Max, 2},
        {PageSize, Max},
        {0, Max},
        {1, Max},
        {2, Max},
    };
    for (const auto& [address, size] : ranges) {
        CAPTURE(address, size);
        WrittenPages pages;
        pages.MarkReference(address, size);
        Tegra::Nx2MarkGpuWritten(address, size);
        pages.Check();
        Tegra::Nx2MarkGpuWritten(address, size);
        pages.Check();
    }
}

TEST_CASE("GPU-written page marking matches overlapping and seeded random ranges",
          "[video_core][nx2_written_pages]") {
    WrittenPages pages;
    std::mt19937_64 random{0x62616e616e7a61};
    for (u32 i = 0; i < 1024; ++i) {
        const u64 address = i % 2 == 0 ? random() % AddressLimit : random() % (256 * PageSize);
        const u64 size = random() % (512 * PageSize);
        pages.MarkReference(address, size);
        Tegra::Nx2MarkGpuWritten(address, size);
    }
    pages.Check();
}

TEST_CASE("Concurrent overlapping GPU-written marks retain every writer's bits",
          "[video_core][nx2_written_pages]") {
    constexpr size_t Writers = 8;
    for (u64 round = 0; round < 64; ++round) {
        WrittenPages pages;
        std::barrier ready{static_cast<std::ptrdiff_t>(Writers + 1)};
        std::vector<std::jthread> writers;
        for (u64 writer = 0; writer < Writers; ++writer) {
            const u64 address = (round * 256 + writer * 17) * PageSize + 1;
            const u64 size = (writer + 1) * 8 * PageSize;
            pages.MarkReference(address, size);
            writers.emplace_back([&, address, size] {
                ready.arrive_and_wait();
                Tegra::Nx2MarkGpuWritten(address, size);
            });
        }
        ready.arrive_and_wait();
        writers.clear();
        pages.Check();
    }
}

TEST_CASE("Non-writer proofs distinguish full keys despite hash collisions and epoch updates",
          "[video_core][nx2_nonwriter_proof]") {
    WrittenPages reset;
    Tegra::Nx2DrawPrograms first{{0x1000, 0x2000}, 1, 0, true};
    auto collision = first;
    collision.memory_id = 2;
    collision.map_serial = (first.memory_id * 0x9e3779b97f4a7c15ULL) ^
                           (collision.memory_id * 0x9e3779b97f4a7c15ULL);
    REQUIRE(first != collision);
    REQUIRE(Tegra::Nx2NonWriterSet::Hash{}(first) == Tegra::Nx2NonWriterSet::Hash{}(collision));
    CHECK_FALSE(Tegra::Nx2IsNonWriterDraw(first, 7));
    const u64 version = Tegra::Nx2NonWriters().version.load();
    Tegra::Nx2InsertNonWriterDraw(first, 7);
    Tegra::Nx2InsertNonWriterDraw(collision, 8);
    CHECK(Tegra::Nx2IsNonWriterDraw(first, 7));
    CHECK(Tegra::Nx2IsNonWriterDraw(collision, 8));
    CHECK_FALSE(Tegra::Nx2IsNonWriterDraw(first, 8));
    CHECK_FALSE(Tegra::Nx2IsNonWriterDraw(collision, 7));
    Tegra::Nx2InsertNonWriterDraw(first, 9);
    CHECK_FALSE(Tegra::Nx2IsNonWriterDraw(first, 7));
    CHECK(Tegra::Nx2IsNonWriterDraw(first, 9));
    CHECK(Tegra::Nx2IsNonWriterDraw(collision, 8));
    CHECK(Tegra::Nx2NonWriters().version.load() == version + 3);
    for (size_t field = 0; field < 9; ++field) {
        auto missing = first;
        if (field < 6) {
            ++missing.addresses[field];
        } else if (field == 6) {
            ++missing.memory_id;
        } else if (field == 7) {
            ++missing.map_serial;
        } else {
            missing.native = false;
        }
        CHECK_FALSE(Tegra::Nx2IsNonWriterDraw(missing, 9));
    }
}

TEST_CASE("Non-writer proofs retain concurrent insertions and clear between sessions",
          "[video_core][nx2_nonwriter_proof]") {
    WrittenPages reset;
    constexpr size_t Writers = 4;
    std::barrier ready{static_cast<std::ptrdiff_t>(Writers)};
    std::atomic<bool> missing{};
    std::vector<std::jthread> writers;
    for (u64 writer = 0; writer < Writers; ++writer) {
        writers.emplace_back([&, writer] {
            ready.arrive_and_wait();
            for (u64 i = 0; i < 64; ++i) {
                const Tegra::Nx2DrawPrograms key{{0x1000 * (i + 1)}, writer + 100, 1, true};
                Tegra::Nx2InsertNonWriterDraw(key, 15);
                if (!Tegra::Nx2IsNonWriterDraw(key, 15)) {
                    missing.store(true);
                }
            }
        });
    }
    writers.clear();
    CHECK_FALSE(missing.load());
    for (u64 writer = 0; writer < Writers; ++writer) {
        for (u64 i = 0; i < 64; ++i) {
            CHECK(Tegra::Nx2IsNonWriterDraw({{0x1000 * (i + 1)}, writer + 100, 1, true}, 15));
        }
    }
    Tegra::Nx2ClearGpuWrittenPages();
    CHECK_FALSE(Tegra::Nx2IsNonWriterDraw({{0x1000}, 100, 1, true}, 15));
}

TEST_CASE("Process-wide NX2 registries remain stable and clearable across repeated teardowns",
          "[video_core][nx2_nonwriter_proof]") {
    WrittenPages reset;

    auto* const non_writers = &Tegra::Nx2NonWriters();
    auto* const semaphore_waiters = &Tegra::Nx2SemaphoreWaiters::Get();
    auto* const sparse_map = &Tegra::Nx2LegacySparseRegionMap();
    auto* const sparse_backing = &Tegra::Nx2LegacySparseBackingStorage();

    const Tegra::Nx2DrawPrograms key{{0x4000, 0x8000}, 7, 2, true};
    Tegra::Nx2InsertNonWriterDraw(key, 21);
    CHECK(Tegra::Nx2IsNonWriterDraw(key, 21));

    int waiter_owner = 0;
    semaphore_waiters->Add(&waiter_owner, 0x10000, 1, Tegra::Nx2SemaphoreWaiters::AcquireEqual);
    CHECK(semaphore_waiters->Size() == 1);

    Tegra::Nx2AddSparseSpan(0x200000, 0x20000);
    Tegra::Nx2SparseRegionCreated(0x200000, 0x20000, 0x10000);
    CHECK(Tegra::Nx2InSparseRegion(0x200000, 0x10000));

    VideoCommon::Nx2GpuLatency::NotePresent();
    VideoCommon::Nx2GpuLatency::Reset();

    for (int cycle = 0; cycle < 2; ++cycle) {
        Tegra::Nx2ClearGpuWrittenPages();
        Tegra::Nx2ClearSparseRegions();
        semaphore_waiters->Clear();
        VideoCommon::Nx2GpuLatency::Reset();

        CHECK(&Tegra::Nx2NonWriters() == non_writers);
        CHECK(&Tegra::Nx2SemaphoreWaiters::Get() == semaphore_waiters);
        CHECK(&Tegra::Nx2LegacySparseRegionMap() == sparse_map);
        CHECK(&Tegra::Nx2LegacySparseBackingStorage() == sparse_backing);
        CHECK_FALSE(Tegra::Nx2IsNonWriterDraw(key, 21));
        CHECK(semaphore_waiters->Size() == 0);
        CHECK_FALSE(Tegra::Nx2InSparseRegion(0x200000, 0x10000));
    }
}
