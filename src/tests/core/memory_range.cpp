// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <limits>
#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"

namespace {

void MapMemoryPage(Common::PageTable& table, u64 page, u8* host) {
    auto& entry = table.entries.GetAndFault(page);
    entry.Store(false, Common::PageType::Memory, 1,
                reinterpret_cast<uintptr_t>(host) - (page << 12));
}

struct TestMemoryProcess {
    TestMemoryProcess() {
        system.Initialize();
        auto& table = process.GetPageTable().GetImpl();
        table.Resize(20, 12);
        u8* const backing = system.DeviceMemory().buffer.BackingBasePointer() + 0x10000;
        std::memset(backing, 0, 0x2000);
        MapMemoryPage(table, 4, backing);
        MapMemoryPage(table, 5, backing + 0x1000);
        process.GetMemory().SetCurrentPageTable(process);
    }

    ~TestMemoryProcess() {
        system.Kernel().UnregisterKernelObject(&process);
    }

    ScopedSystemTestContext context;
    Core::System system;
    Kernel::KProcess process{system.Kernel()};
};

} // namespace

TEST_CASE("IsValidVirtualAddressRange rejects wrapping and unmapped ranges", "[core][memory]") {
    TestMemoryProcess test;
    auto& memory = test.process.GetMemory();

    // Valid mapped single-page, cross-page, and zero-sized ranges.
    CHECK(memory.IsValidVirtualAddressRange(0x4000, 0));
    CHECK(memory.IsValidVirtualAddressRange(0x4000, sizeof(u64)));
    CHECK(memory.IsValidVirtualAddressRange(0x4ff8, 16));

    // Unmapped boundary page (page 6 is unmapped).
    CHECK_FALSE(memory.IsValidVirtualAddressRange(0x5ff8, 16));
    CHECK_FALSE(memory.IsValidVirtualAddressRange(0x6000, sizeof(u64)));

    // 64-bit address wraparound cases that previously skipped the page check loop.
    CHECK_FALSE(memory.IsValidVirtualAddressRange(0xfffffffffffffff8ULL, sizeof(u64)));
    CHECK_FALSE(memory.IsValidVirtualAddressRange(0xfffffffffffffffcULL, sizeof(u64)));
    CHECK_FALSE(memory.IsValidVirtualAddressRange(0xfffffffffffff000ULL, 0x2000));
    CHECK_FALSE(memory.IsValidVirtualAddressRange(0xffffffffffffffffULL, 1));
    CHECK_FALSE(memory.IsValidVirtualAddressRange(0xffffffffffffffffULL, sizeof(u64)));
    CHECK_FALSE(memory.IsValidVirtualAddressRange(0x8000000000000000ULL, 0x8000000000000000ULL));
}
