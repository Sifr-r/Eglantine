// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Vulkan {

/// Images with more texels than this are candidates for the unbacked sparse fallback.
inline constexpr u64 UnbackedImageTexelThreshold = u64{1} << 28;

/// Whether an image must take the unbacked sparse fallback instead of being given real memory.
///
/// An image with nothing bound drops its writes and reads back zero, so what counts is the memory
/// the driver needs for it, not its texel count: a large depth map (Ocarina of Time's sun shadow
/// map, 18432x18432 D16 at 1.5x resolution) fits easily while a 4.5G-texel sparse texture array
/// never does. Images that need more than a quarter of the device-local memory are not backed.
/// `required_bytes` is zero when the driver could not report the requirement.
constexpr bool ShouldLeaveImageUnbacked(u64 texels, u64 required_bytes, u64 device_local_bytes) {
    if (texels <= UnbackedImageTexelThreshold) {
        return false;
    }
    return required_bytes == 0 || required_bytes > device_local_bytes / 4;
}

/// Whether unbacked sparse images guarantee zero-reads on non-resident blocks and do not fault
/// on the given driver. `forced_env` is the first character of `EDEN_NX2_SPARSE_UNBACKED`
/// ('0', '1', or '\0').
constexpr bool CanUseUnbackedSparseImagesPolicy(bool residency_non_resident_strict,
                                              bool is_amd_or_radv, char forced_env) noexcept {
    if (forced_env == '0' || forced_env == '1') {
        return forced_env == '1';
    }
    return residency_non_resident_strict && !is_amd_or_radv;
}

} // namespace Vulkan
