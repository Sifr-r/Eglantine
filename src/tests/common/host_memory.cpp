// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "common/host_memory.h"
#include "common/literals.h"

using Common::HostMemory;
using namespace Common::Literals;

static constexpr size_t VIRTUAL_SIZE = 1ULL << 39;
static constexpr size_t BACKING_SIZE = 4_GiB;
static constexpr auto PERMS = Common::MemoryPermission::ReadWrite;
static constexpr auto HEAP = false;

TEST_CASE("HostMemory: Initialize and deinitialize", "[common]") {
    {
        HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
        REQUIRE(mem.BackingBasePointer() != nullptr);
    }
    {
        HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
        REQUIRE(mem.BackingBasePointer() != nullptr);
    }
}

TEST_CASE("HostMemory: Simple map", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x8000, 0x1000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[0] = 50;
    REQUIRE(data[0] == 50);
}

TEST_CASE("HostMemory: Simple mirror map", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);
    mem.Map(0x8000, 0x4000, 0x1000, PERMS, HEAP);

    volatile u8* const mirror_a = mem.VirtualBasePointer() + 0x5000;
    volatile u8* const mirror_b = mem.VirtualBasePointer() + 0x8000;
    mirror_b[0] = 76;
    REQUIRE(mirror_a[0x1000] == 76);
}

TEST_CASE("HostMemory: Simple unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[75] = 50;
    REQUIRE(data[75] == 50);

    mem.Unmap(0x5000, 0x2000, HEAP);
}

TEST_CASE("HostMemory: Simple unmap and remap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[0] = 50;
    REQUIRE(data[0] == 50);

    mem.Unmap(0x5000, 0x2000, HEAP);

    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);
    REQUIRE(data[0] == 50);

    mem.Map(0x7000, 0x2000, 0x5000, PERMS, HEAP);
    REQUIRE(data[0x3000] == 50);
}

TEST_CASE("HostMemory: Nieche allocation", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x20000, PERMS, HEAP);
    mem.Unmap(0x0000, 0x4000, HEAP);
    mem.Map(0x1000, 0, 0x2000, PERMS, HEAP);
    mem.Map(0x3000, 0, 0x1000, PERMS, HEAP);
    mem.Map(0, 0, 0x1000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Full unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x8000, 0x4000, HEAP);
    mem.Map(0x6000, 0, 0x16000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Right out of bounds unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x2000, 0x4000, HEAP);
    mem.Map(0x2000, 0x80000, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Left out of bounds unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);

    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x6000, 0x4000, HEAP);
    mem.Map(0x8000, 0, 0x2000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Multiple placeholder unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x1b000, PERMS, HEAP);
    mem.Unmap(0x3000, 0x1c000, HEAP);
    mem.Map(0x3000, 0, 0x20000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap between placeholders", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x2000, 0x4000, HEAP);
    mem.Map(0x2000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap to origin", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x4000, 0x4000, HEAP);
    mem.Map(0, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap to right", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x8000, 0x4000, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Partial right unmap check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x1000] = 17;

    mem.Unmap(0x6000, 0x2000, HEAP);

    REQUIRE(ptr[0x1000] == 17);
}

TEST_CASE("HostMemory: Partial left unmap check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x3000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x4000, 0x2000, HEAP);

    REQUIRE(ptr[0x3000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

TEST_CASE("HostMemory: Partial middle unmap check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x0000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x1000, 0x2000, HEAP);

    REQUIRE(ptr[0x0000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

TEST_CASE("HostMemory: Partial sparse middle unmap and check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x2000, PERMS, HEAP);
    mem.Map(0x6000, 0x20000, 0x2000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x0000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x5000, 0x2000, HEAP);

    REQUIRE(ptr[0x0000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

#ifdef _WIN32
namespace {

u64 AvailableCommitBytes() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    REQUIRE(GlobalMemoryStatusEx(&status) != FALSE);
    return status.ullAvailPageFile;
}

MEMORY_BASIC_INFORMATION QueryMemory(const void* addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    REQUIRE(VirtualQuery(addr, &mbi, sizeof(mbi)) != 0);
    return mbi;
}

#ifdef _MSC_VER
bool TryReadFaults(const volatile u8* ptr) noexcept {
    __try {
        [[maybe_unused]] const volatile u8 value = *ptr;
        return false;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
}

bool TryWriteFaults(volatile u8* ptr, u8 value) noexcept {
    __try {
        *ptr = value;
        return false;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
}
#endif

} // Anonymous namespace
#endif

TEST_CASE("HostMemory: 14 GiB backing reserves without upfront commit and commits on demand",
          "[common][host_memory]") {
#ifdef _WIN32
    const u64 commit_before = AvailableCommitBytes();
#endif
    HostMemory mem(14_GiB, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    REQUIRE(mem.VirtualBasePointer() != nullptr);

#ifdef _WIN32
    const u64 commit_after = AvailableCommitBytes();
    const u64 commit_delta = commit_before > commit_after ? commit_before - commit_after : 0;
    CHECK(commit_delta < 128_MiB);

    const auto initial_mbi = QueryMemory(mem.BackingBasePointer());
    CHECK(initial_mbi.State == MEM_RESERVE);
    CHECK(initial_mbi.Type == MEM_MAPPED);

    volatile u8* const backing_touch = mem.BackingBasePointer() + 0x100000;
    *backing_touch = 0x42;
    CHECK(*backing_touch == 0x42);

    const auto touched_mbi = QueryMemory(mem.BackingBasePointer() + 0x100000);
    CHECK(touched_mbi.State == MEM_COMMIT);

    const auto untouched_mbi = QueryMemory(mem.BackingBasePointer() + 1_GiB);
    CHECK(untouched_mbi.State == MEM_RESERVE);
#endif

    mem.Map(0x20000, 0x200000, 0x10000, PERMS, HEAP);
    volatile u8* const view_ptr = mem.VirtualBasePointer() + 0x20000;
    *view_ptr = 0x7A;
    CHECK(mem.BackingBasePointer()[0x200000] == 0x7A);
}

TEST_CASE("HostMemory: Protect handles uncommitted and mixed ranges without dropping protections",
          "[common][host_memory]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    REQUIRE(mem.VirtualBasePointer() != nullptr);

    mem.Map(0x40000, 0x300000, 0x10000, PERMS, HEAP);
    mem.Protect(0x40000, 0x10000, Common::MemoryPermission::Read);

#ifdef _WIN32
    const auto view_ro = QueryMemory(mem.VirtualBasePointer() + 0x40000);
    CHECK(view_ro.State == MEM_COMMIT);
    CHECK(view_ro.Protect == PAGE_READONLY);

    // Committing/writing through the backing view must not drop PAGE_READONLY on the alias view.
    mem.BackingBasePointer()[0x300000] = 0x33;
    CHECK(mem.VirtualBasePointer()[0x40000] == 0x33);
    CHECK(QueryMemory(mem.VirtualBasePointer() + 0x40000).Protect == PAGE_READONLY);

    mem.Protect(0x40000, 0x10000, Common::MemoryPermission{});
    const auto view_noaccess = QueryMemory(mem.VirtualBasePointer() + 0x40000);
    CHECK(view_noaccess.State == MEM_COMMIT);
    CHECK(view_noaccess.Protect == PAGE_NOACCESS);

    mem.Protect(0x40000, 0x10000, Common::MemoryPermission::ReadWrite);
    const auto view_rw = QueryMemory(mem.VirtualBasePointer() + 0x40000);
    CHECK(view_rw.State == MEM_COMMIT);
    CHECK(view_rw.Protect == PAGE_READWRITE);
    mem.VirtualBasePointer()[0x40000] = 0x55;
    CHECK(mem.BackingBasePointer()[0x300000] == 0x55);

    // Restoring ReadWrite over a mixed (committed + uncommitted) view must leave untouched pages
    // uncommitted.
    mem.Map(0x60000, 0x400000, 0x20000, PERMS, HEAP);
    mem.VirtualBasePointer()[0x60000] = 0x11;
    mem.Protect(0x60000, 0x20000, Common::MemoryPermission::ReadWrite);
    CHECK(QueryMemory(mem.VirtualBasePointer() + 0x60000).State == MEM_COMMIT);
    CHECK(QueryMemory(mem.VirtualBasePointer() + 0x70000).State == MEM_RESERVE);
#endif
}

TEST_CASE("HostMemory: Unmapped placeholder and protection faults propagate to SEH",
          "[common][host_memory]") {
#if defined(_WIN32) && defined(_MSC_VER)
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.VirtualBasePointer() != nullptr);

    // Unmapped placeholders inside virtual_base must not be committed or swallowed by VEH.
    CHECK(TryReadFaults(mem.VirtualBasePointer() + 0x100000));

    mem.Map(0x120000, 0x500000, 0x10000, PERMS, HEAP);
    mem.Protect(0x120000, 0x10000, Common::MemoryPermission::Read);
    CHECK(TryWriteFaults(mem.VirtualBasePointer() + 0x120000, 0x99));

    mem.Protect(0x120000, 0x10000, Common::MemoryPermission{});
    CHECK(TryReadFaults(mem.VirtualBasePointer() + 0x120000));
#else
    SUCCEED("Windows SEH propagation test applies to Windows MSVC builds");
#endif
}

TEST_CASE("HostMemory: Concurrent first-touch on shared uncommitted pages",
          "[common][host_memory]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    REQUIRE(mem.VirtualBasePointer() != nullptr);

    constexpr size_t NumGranules = 32;
    constexpr size_t GranuleBytes = 0x10000;
    constexpr size_t BaseOffset = 0x600000;
    mem.Map(BaseOffset, BaseOffset, NumGranules * GranuleBytes, PERMS, HEAP);

    constexpr int NumThreads = 8;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<int> mismatches{0};
    std::vector<std::thread> workers;
    workers.reserve(NumThreads);

    for (int t = 0; t < NumThreads; ++t) {
        workers.emplace_back([&, t] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const bool use_view = (t & 1) != 0;
            u8* const base =
                (use_view ? mem.VirtualBasePointer() : mem.BackingBasePointer()) + BaseOffset;
            for (size_t g = 0; g < NumGranules; ++g) {
                volatile u8* const cell = base + g * GranuleBytes + static_cast<size_t>(t) * 64;
                const u8 expected = static_cast<u8>((g + t) & 0xFF);
                *cell = expected;
                if (*cell != expected) {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != NumThreads) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);

    for (auto& worker : workers) {
        worker.join();
    }
    CHECK(mismatches.load() == 0);
}

TEST_CASE("HostMemory: ClearBackingRegion skips uncommitted zero pages and handles non-zero fill",
          "[common][host_memory]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);

    mem.ClearBackingRegion(0x800000, 1_MiB, 0);
    mem.MarkDirty(0x800000, 0x10000);
    mem.ClearBackingRegion(0x800000, 0x10000, 0);

#ifdef _WIN32
    CHECK(QueryMemory(mem.BackingBasePointer() + 0x800000).State == MEM_RESERVE);
#endif

    mem.ClearBackingRegion(0x900000, 0x10000, 0x5A);
    CHECK(mem.BackingBasePointer()[0x900000] == 0x5A);
    CHECK(mem.BackingBasePointer()[0x90FFFF] == 0x5A);

    mem.ClearBackingRegion(0x900000, 0x10000, 0);
    CHECK(mem.BackingBasePointer()[0x900000] == 0);
    CHECK(mem.BackingBasePointer()[0x90FFFF] == 0);

#ifdef _WIN32
    const size_t ensure_offset = 0xA00000;
    CHECK(QueryMemory(mem.BackingBasePointer() + ensure_offset).State == MEM_RESERVE);
    REQUIRE(Common::EnsureHostMemoryCommitted(mem.BackingBasePointer() + ensure_offset, 0x2000));
    CHECK(QueryMemory(mem.BackingBasePointer() + ensure_offset).State == MEM_COMMIT);
#endif
}

TEST_CASE("HostMemory: Fallback allocation releases cleanly and move semantics prevent double-free",
          "[common][host_memory]") {
    HostMemory moved_src(64_KiB, VIRTUAL_SIZE);
    u8* const orig_backing = moved_src.BackingBasePointer();
    REQUIRE(orig_backing != nullptr);

    HostMemory moved_dst(std::move(moved_src));
    CHECK(moved_dst.BackingBasePointer() == orig_backing);
    CHECK(moved_src.BackingBasePointer() == nullptr);
    CHECK(moved_src.VirtualBasePointer() == nullptr);

    HostMemory assigned_dst(64_KiB, VIRTUAL_SIZE);
    assigned_dst = std::move(moved_dst);
    CHECK(assigned_dst.BackingBasePointer() == orig_backing);
    CHECK(moved_dst.BackingBasePointer() == nullptr);

#ifdef _WIN32
    // Force VirtualAlloc2 placeholder reservation to fail so HostMemory exercises the Windows
    // fallback allocator, VEH on-demand commit, move transfer, and VirtualFree(..., 0, MEM_RELEASE).
    u8* saved_fallback_ptr = nullptr;
    {
        HostMemory fallback_a(64_KiB, 1ULL << 52);
        REQUIRE(fallback_a.BackingBasePointer() != nullptr);
        CHECK(fallback_a.VirtualBasePointer() == nullptr);
        saved_fallback_ptr = fallback_a.BackingBasePointer();

        CHECK(QueryMemory(saved_fallback_ptr).State == MEM_RESERVE);
        fallback_a.BackingBasePointer()[0x123] = 0xAB;
        CHECK(fallback_a.BackingBasePointer()[0x123] == 0xAB);
        CHECK(QueryMemory(saved_fallback_ptr).State == MEM_COMMIT);

        HostMemory fallback_b(std::move(fallback_a));
        CHECK(fallback_a.BackingBasePointer() == nullptr);
        CHECK(fallback_b.BackingBasePointer() == saved_fallback_ptr);
        CHECK(fallback_b.BackingBasePointer()[0x123] == 0xAB);
    }
    REQUIRE(saved_fallback_ptr != nullptr);
    CHECK(QueryMemory(saved_fallback_ptr).State == MEM_FREE);
#endif
}
