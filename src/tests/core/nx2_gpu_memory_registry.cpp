// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"

TEST_CASE("NX2 sparse aliases cover the entire tile backing", "[nx2_memory]") {
    auto& registry = Service::Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
    constexpr u64 base = 0xf0000000;
    constexpr u64 device = 0x90000000;
    registry.RecordDeviceRange(base, 0x4000, device);
    registry.RecordDeviceRange(base + 0x1000, 0x1000, device + 0x10000);
    SCOPE_EXIT {
        registry.ReleaseBacking(base, 0x4000);
        registry.ReleaseBacking(base + 0x1000, 0x1000);
    };

    // A small tile fits the most recent mapping; a tile crossing its end must use the outer one.
    CHECK(registry.DeviceAddressOf(base + 0x1800, 0x100) == device + 0x10800);
    CHECK(registry.DeviceAddressOf(base + 0x1800, 0x1000) == device + 0x1800);
    CHECK(registry.DeviceAddressOf(base, 0x4000) == device);
    CHECK_FALSE(registry.DeviceAddressOf(base + 0x3000, 0x2000));
    CHECK_FALSE(registry.DeviceAddressOf(base, 0));
    CHECK_FALSE(registry.DeviceAddressOf(base, std::numeric_limits<u64>::max()));
    CHECK_FALSE(registry.DeviceAddressOf(std::numeric_limits<u64>::max(), 1));
}

TEST_CASE("NX2 released backing cannot supply a sparse alias", "[nx2_memory]") {
    auto& registry = Service::Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
    constexpr u64 base = 0xf1000000;
    registry.RecordDeviceRange(base, 0x1000, 0xa0000000);
    REQUIRE(registry.DeviceAddressOf(base, 0x1000));
    registry.ReleaseBacking(base, 0x1000);
    CHECK_FALSE(registry.DeviceAddressOf(base, 1));
}

TEST_CASE("NX2 releasing a backing span clears every contained sub-range", "[nx2_memory]") {
    auto& registry = Service::Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
    constexpr u64 base = 0xf2000000;
    constexpr u64 dev = 0xb0000000;

    registry.RecordDeviceRange(base - 0x2000, 0x1000, dev + 0x00000);
    registry.RecordDeviceRange(base - 0x1000, 0x2000, dev + 0x10000);
    registry.RecordDeviceRange(base, 0x1000, dev + 0x20000);
    registry.RecordDeviceRange(base + 0x2000, 0x1000, dev + 0x30000);
    registry.RecordDeviceRange(base + 0x4000, 0x2000, dev + 0x40000);
    registry.RecordDeviceRange(base + 0x6000, 0x4000, dev + 0x50000);
    registry.RecordDeviceRange(base + 0xa000, 0x1000, dev + 0x60000);

    int release_token{};
    u64 notified_addr{};
    u64 notified_size{};
    registry.AddListener(
        &release_token, [](u64, u64, u64) {}, [](u64, u64) {},
        [&](u64 cpu_address, u64 size) {
            notified_addr = cpu_address;
            notified_size = size;
        });

    SCOPE_EXIT {
        registry.RemoveListener(&release_token);
        registry.ReleaseBacking(base - 0x2000, 0xd000);
    };

    registry.ReleaseBacking(base, 0x8000);

    CHECK(notified_addr == base);
    CHECK(notified_size == 0x8000);
    CHECK_FALSE(registry.DeviceAddressOf(base + 0x2000, 0x1000));
    CHECK_FALSE(registry.DeviceAddressOf(base + 0x4000, 0x2000));
    // Exact-start range covered by [base, base + 0x8000) is gone; overlapping-start before base stays.
    CHECK(registry.DeviceAddressOf(base, 0x100) == dev + 0x11000);
    CHECK(registry.DeviceAddressOf(base - 0x2000, 0x1000) == dev + 0x00000);
    CHECK(registry.DeviceAddressOf(base - 0x1000, 0x2000) == dev + 0x10000);
    CHECK(registry.DeviceAddressOf(base + 0x6000, 0x4000) == dev + 0x50000);
    CHECK(registry.DeviceAddressOf(base + 0xa000, 0x1000) == dev + 0x60000);
}

TEST_CASE("NX2 span release handles zero size and top-of-address-space bounds", "[nx2_memory]") {
    auto& registry = Service::Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
    constexpr u64 base = std::numeric_limits<u64>::max() - 0x1fff;
    constexpr u64 dev = 0xc0000000;

    registry.RecordDeviceRange(base, 0x1000, dev);
    registry.RecordDeviceRange(base + 0x1000, 0x1000, dev + 0x1000);
    SCOPE_EXIT {
        registry.ReleaseBacking(base, 0x2000);
    };

    registry.ReleaseBacking(base, 0);
    CHECK(registry.DeviceAddressOf(base, 0x1000) == dev);
    CHECK(registry.DeviceAddressOf(base + 0x1000, 0x1000) == dev + 0x1000);

    registry.ReleaseBacking(base, 0x2000);
    CHECK_FALSE(registry.DeviceAddressOf(base, 0x1000));
    CHECK_FALSE(registry.DeviceAddressOf(base + 0x1000, 0x1000));
}
