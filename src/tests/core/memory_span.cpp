// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#ifndef _WIN32
#include <sys/mman.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"

TEST_CASE("Guest memory spans validate every covered page", "[core][memory_span]") {
    ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    Kernel::KProcess process{system.Kernel()};
    SCOPE_EXIT {
        system.Kernel().UnregisterKernelObject(&process);
    };

    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, Core::Memory::YUZU_PAGEBITS);
    auto& memory = process.GetMemory();
    memory.SetCurrentPageTable(process);
    u8* const backing = system.DeviceMemory().buffer.BackingBasePointer();
    constexpr auto page_size = Core::Memory::YUZU_PAGESIZE;
    const auto map_page = [&](std::size_t page, u16 mapping_id, u64 host_page) {
        const auto host = reinterpret_cast<uintptr_t>(backing + host_page * page_size);
        table.entries.GetAndFault(page).Store(false, Common::PageType::Memory, mapping_id,
                                              host - static_cast<uintptr_t>(page * page_size));
    };

    // Matching block IDs on endpoints must still reject a remapped non-contiguous middle page.
    map_page(4, 1, 16);
    map_page(5, 2, 32);
    map_page(6, 1, 18);
    CHECK(memory.GetSpan(4 * page_size, 2 * page_size + 1) == nullptr);
    CHECK(std::as_const(memory).GetSpan(4 * page_size, 2 * page_size + 1) == nullptr);

    // Separate mappings whose host pages are contiguous form a valid span up to page boundaries.
    map_page(5, 3, 17);
    map_page(7, 4, 48);
    CHECK(memory.GetSpan(4 * page_size, 3 * page_size) == backing + 16 * page_size);
    CHECK(std::as_const(memory).GetSpan(4 * page_size, 3 * page_size) == backing + 16 * page_size);
    CHECK(memory.GetSpan(4 * page_size + 1, 3 * page_size - 1) == backing + 16 * page_size + 1);
    CHECK(std::as_const(memory).GetSpan(4 * page_size + 1, 3 * page_size - 1) ==
          backing + 16 * page_size + 1);
    CHECK(memory.GetSpan(4 * page_size, 3 * page_size + 1) == nullptr);
    CHECK(std::as_const(memory).GetSpan(4 * page_size, 3 * page_size + 1) == nullptr);
    CHECK(memory.GetSpan(7 * page_size - 3, 5) == nullptr);
    CHECK(std::as_const(memory).GetSpan(7 * page_size - 3, 5) == nullptr);
    CHECK(memory.GetSpan(7 * page_size - 3, 3) == backing + 19 * page_size - 3);
    CHECK(std::as_const(memory).GetSpan(7 * page_size - 3, 3) == backing + 19 * page_size - 3);

    // Rasterizer-cached and debug markers preserve host contiguity.
    table.entries.GetAndFault(5).MarkRasterizerCached();
    CHECK(memory.GetSpan(4 * page_size, 3 * page_size) == backing + 16 * page_size);
    table.entries.GetAndFault(4).MarkRasterizerCached();
    CHECK(memory.GetSpan(4 * page_size, 3 * page_size) == backing + 16 * page_size);
    CHECK(std::as_const(memory).GetSpan(4 * page_size, 3 * page_size) == backing + 16 * page_size);

    const auto [ptr6, type6, block6] = table.entries[6].PointerTypeBlock(true);
    REQUIRE(type6 == Common::PageType::Memory);
    table.entries.GetAndFault(6).MarkDebug(ptr6, block6);
    CHECK(memory.GetSpan(4 * page_size, 3 * page_size) == backing + 16 * page_size);
    CHECK(std::as_const(memory).GetSpan(4 * page_size, 3 * page_size) == backing + 16 * page_size);
    CHECK(memory.GetSpan(6 * page_size, page_size) == backing + 18 * page_size);
    CHECK(std::as_const(memory).GetSpan(6 * page_size, page_size) == backing + 18 * page_size);

    // Unmapped pages in the middle, at the start, or at the tail invalidate the span.
    table.entries.GetAndFault(5).Store(false, Common::PageType::Unmapped, 1, 0);
    CHECK(memory.GetSpan(4 * page_size, 3 * page_size) == nullptr);
    CHECK(std::as_const(memory).GetSpan(4 * page_size, 3 * page_size) == nullptr);

    CHECK(memory.GetSpan(3 * page_size, 2 * page_size) == nullptr);
    CHECK(std::as_const(memory).GetSpan(3 * page_size, 2 * page_size) == nullptr);

    CHECK(memory.GetSpan(7 * page_size, 2 * page_size) == nullptr);
    CHECK(std::as_const(memory).GetSpan(7 * page_size, 2 * page_size) == nullptr);

    CHECK(memory.GetSpan(10 * page_size, 4 * page_size) == nullptr);
    CHECK(std::as_const(memory).GetSpan(10 * page_size, 4 * page_size) == nullptr);

    // Zero-size spans succeed on mapped pages and fail on unmapped pages.
    CHECK(memory.GetSpan(4 * page_size, 0) == backing + 16 * page_size);
    CHECK(std::as_const(memory).GetSpan(4 * page_size, 0) == backing + 16 * page_size);
    CHECK(memory.GetSpan(3 * page_size, 0) == nullptr);
    CHECK(std::as_const(memory).GetSpan(3 * page_size, 0) == nullptr);

    // Integer overflow and out-of-range spans must be rejected without wraparound.
    CHECK(memory.GetSpan(0xfffffffffffffff8ULL, 8) == nullptr);
    CHECK(std::as_const(memory).GetSpan(0xfffffffffffffff8ULL, 8) == nullptr);
    CHECK(memory.GetSpan(0xfffffffffffff000ULL, 0x2000) == nullptr);
    CHECK(std::as_const(memory).GetSpan(0xfffffffffffff000ULL, 0x2000) == nullptr);
    CHECK(memory.GetSpan(0xffffffffffffffffULL, 1) == nullptr);
    CHECK(memory.GetSpan(0xffffffffffffffffULL, 0) == nullptr);
    CHECK(memory.GetSpan(0x8000000000000000ULL, 0x8000000000000000ULL) == nullptr);
    CHECK(memory.GetSpan(4 * page_size, (std::numeric_limits<std::size_t>::max)()) == nullptr);
    CHECK(std::as_const(memory).GetSpan(4 * page_size, (std::numeric_limits<std::size_t>::max)()) ==
          nullptr);
    CHECK(memory.GetSpan(0, (std::numeric_limits<std::size_t>::max)()) == nullptr);
}

TEST_CASE("An empty guest span at the exclusive end has no page entry",
          "[core][memory_span][memory_span_boundary]") {
    ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    Kernel::KProcess process{system.Kernel()};
    SCOPE_EXIT {
        system.Kernel().UnregisterKernelObject(&process);
    };

    auto& table = process.GetPageTable().GetImpl();
    const std::size_t entries_per_host_page =
        static_cast<std::size_t>(Common::HostPageSize) / sizeof(Common::PageTable::PageEntryData);
    const u32 address_bits = static_cast<u32>(
        Core::Memory::YUZU_PAGEBITS +
        static_cast<std::size_t>(std::countr_zero(entries_per_host_page)));
    table.Resize(address_bits, Core::Memory::YUZU_PAGEBITS);
#ifndef _WIN32
    table.entries.ResizeAndClear(entries_per_host_page * 2);
    const auto* guard = table.entries.data() + entries_per_host_page;
    REQUIRE(mprotect(const_cast<Common::PageTable::PageEntryData*>(guard),
                     static_cast<std::size_t>(Common::HostPageSize), PROT_NONE) == 0);
#endif
    constexpr auto page_size = Core::Memory::YUZU_PAGESIZE;
    const u64 end = u64{1} << address_bits;
    auto& memory = process.GetMemory();
    memory.SetCurrentPageTable(process);
    u8* const backing = system.DeviceMemory().buffer.BackingBasePointer();
    table.entries.GetAndFault(entries_per_host_page - 1)
        .Store(false, Common::PageType::Memory, 1,
               reinterpret_cast<uintptr_t>(backing) - static_cast<uintptr_t>(end - page_size));

    CHECK(memory.GetSpan(end - 1, 0) == backing + page_size - 1);
    CHECK(std::as_const(memory).GetSpan(end - 1, 0) == backing + page_size - 1);
    CHECK(memory.GetSpan(end - 1, 1) == backing + page_size - 1);
    CHECK(std::as_const(memory).GetSpan(end - 1, 1) == backing + page_size - 1);
    CHECK(memory.GetSpan(end - page_size, page_size) == backing);
    CHECK(std::as_const(memory).GetSpan(end - page_size, page_size) == backing);
    CHECK(memory.GetSpan(end - 1, 2) == nullptr);
    CHECK(std::as_const(memory).GetSpan(end - 1, 2) == nullptr);
    CHECK(memory.GetSpan(end, 1) == nullptr);
    CHECK(std::as_const(memory).GetSpan(end, 1) == nullptr);
    CHECK(memory.GetSpan(end, 0) == nullptr);
    CHECK(std::as_const(memory).GetSpan(end, 0) == nullptr);
}
