// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "common/common_types.h"

namespace Tegra::Texture::ETC2 {

/// The ETC2 / EAC sub-formats the Switch can present.
enum class Format {
    RGB,          ///< 8 bytes per 4x4 block, opaque colour.
    RGB_PTA,      ///< 8 bytes per 4x4 block, punchthrough alpha.
    RGBA,         ///< 16 bytes per 4x4 block, ETC2 colour plus ETC2 alpha.
    R11,          ///< 8 bytes per 4x4 block, unsigned EAC single channel.
    R11_SNORM,    ///< 8 bytes per 4x4 block, signed EAC single channel.
    R11G11,       ///< 16 bytes per 4x4 block, unsigned EAC two channel.
    R11G11_SNORM, ///< 16 bytes per 4x4 block, signed EAC two channel.
};

/// Bytes a single decoded texel occupies for `format`.
[[nodiscard]] u32 BytesPerTexel(Format format);

/// Decodes ETC2/EAC blocks into RGBA8 (or R8 / R8G8 for the EAC formats).
///
/// `data` holds the blocks of one extent: `depth` slices of ceil(width / 4) by ceil(height / 4)
/// blocks in row major order, the slices laid out back to back. `output` must hold
/// width * height * depth * BytesPerTexel(format) bytes.
void Decompress(std::span<const u8> data, Format format, u32 width, u32 height, u32 depth,
                std::span<u8> output);

} // namespace Tegra::Texture::ETC2
