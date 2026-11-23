// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string_view>

#include "common/common_types.h"

namespace Loader {
class AppLoader;
}

namespace Core {

class System;

enum class PlatformKind : u8 {
    Switch1,
    Switch2,
};

enum class PlatformResolution : u8 {
    DefaultSwitch1,   ///< No main.npdm available (e.g. NRO/NSO/KIP homebrew or uninitialized session)
    ConfirmedSwitch1, ///< Loader resolved effective ExeFS main.npdm -> Switch 1 ACID layout
    ConfirmedSwitch2, ///< Loader resolved effective ExeFS main.npdm -> Switch 2 ACID layout
    OverrideSwitch1,  ///< Forced via EDEN_NX2_PLATFORM=1
    OverrideSwitch2,  ///< Forced via EDEN_NX2_PLATFORM=2
};

struct PlatformProfile {
    PlatformKind kind{PlatformKind::Switch1};
    PlatformResolution resolution{PlatformResolution::DefaultSwitch1};
    u64 guest_cntfrq{19'200'000ULL};
    u32 cpu_cores{4};
    u32 device_address_bits{34};
    bool prefer_async_buffer_downloads{true};
    bool force_12gb_memory_layout{false};
    bool reset_dram_on_shutdown{false};
    bool allow_contradictory_nx2_gpu_query{true};

    [[nodiscard]] constexpr bool IsSwitch2() const noexcept {
        return kind == PlatformKind::Switch2;
    }

    [[nodiscard]] constexpr bool IsConfirmed() const noexcept {
        return resolution == PlatformResolution::ConfirmedSwitch1 ||
               resolution == PlatformResolution::ConfirmedSwitch2;
    }
};

[[nodiscard]] PlatformProfile MakePlatformProfile(PlatformResolution resolution) noexcept;

/// Parses EDEN_NX2_PLATFORM ("1" -> OverrideSwitch1, "2" -> OverrideSwitch2, otherwise nullopt).
[[nodiscard]] std::optional<PlatformResolution> ParsePlatformOverride(const char* value) noexcept;

/// Resolves the platform profile from an AppLoader's effective ProgramMetadata and optional override.
[[nodiscard]] PlatformProfile ResolvePlatformProfile(
    System& system, Loader::AppLoader* loader,
    std::optional<PlatformResolution> env_override = std::nullopt);

[[nodiscard]] std::string_view PlatformResolutionName(PlatformResolution resolution) noexcept;

} // namespace Core
