// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"

namespace Vulkan {

class Device;

struct CoopHmmaProfile {
    bool supported{};
    u32 tile_m{16};
    u32 tile_n{8};
    u32 tile_k{16};
    u32 subgroup_size{32};
    std::array<u32, 4> a_word_map{0, 2, 1, 3};
    bool from_cache{};
};

/// Probes dynamic per-lane register mapping for HMMA.16816.F16 cooperative matrices on the device,
/// loading or saving the verified profile in ShaderDir keyed on device/driver/SPIR-V version.
[[nodiscard]] CoopHmmaProfile ProbeCooperativeHmma(const Device& device);

/// Runs the HMMA.16816.F16 cooperative matrix lowering once on known matrices and compares every
/// lane's result. Vulkan leaves the per-lane order of a cooperative matrix's elements to the driver
/// and the lowering relies on it, so a driver with another order keeps the shader emulation.
[[nodiscard]] bool TestCooperativeHmma(const Device& device);

} // namespace Vulkan
