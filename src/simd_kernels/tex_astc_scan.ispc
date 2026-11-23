// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Multi-ISA ASTC opacity prescan matching Tegra::Texture::ASTC::IsOpaque bit-for-bit.
// `mode_table` holds 2048 packed uint32 descriptors:
//   bits  0..15: weight_bits
//   bit   16:    error
//   bit   17:    void_extent
//   bit   18:    void_extent_hdr
//   bit   19:    dual_plane

export uniform bool astc_is_opaque_blocks(const uniform uint8 data[],
                                          const uniform uint32 mode_table[],
                                          uniform uint64 block_count) {
    // Endpoint modes with an alpha channel: 4, 5, 10, 12, 13, 14, 15.
    const uniform uint32 ALPHA_MODES =
        (1u << 4) | (1u << 5) | (1u << 10) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15);

    for (uniform uint64 base = 0; base < block_count; base += (uniform uint64)programCount) {
        const uint64 b = base + (uint64)programIndex;
        bool has_alpha = false;

        if (b < block_count) {
            const uint64 off = b * 16;
            const uint32 low = (uint32)data[off + 0] | ((uint32)data[off + 1] << 8) |
                               ((uint32)data[off + 2] << 16) | ((uint32)data[off + 3] << 24);
            const uint32 packed = mode_table[low & 0x7FFu];
            const bool error = (packed & (1u << 16)) != 0;

            if (!error) {
                const bool void_extent = (packed & (1u << 17)) != 0;
                if (void_extent) {
                    if ((low & 0xC00u) == 0xC00u) {
                        const bool hdr = (packed & (1u << 18)) != 0;
                        const uint32 alpha = (uint32)data[off + 14] | ((uint32)data[off + 15] << 8);
                        if (alpha != (hdr ? 0x3C00u : 0xFFFFu)) {
                            has_alpha = true;
                        }
                    }
                } else {
                    const uint32 partitions = ((low >> 11) & 3u) + 1u;
                    const bool dual_plane = (packed & (1u << 19)) != 0;
                    if (!(partitions == 4u && dual_plane)) {
                        if (partitions == 1u) {
                            const uint32 mode = (low >> 13) & 0xFu;
                            if (((ALPHA_MODES >> mode) & 1u) != 0) {
                                has_alpha = true;
                            }
                        } else {
                            const uint32 base_cem = (low >> 23) & 0x3Fu;
                            const uint32 base_mode = base_cem & 3u;
                            if (base_mode == 0u) {
                                if (((ALPHA_MODES >> (base_cem >> 2)) & 1u) != 0) {
                                    has_alpha = true;
                                }
                            } else {
                                const uint32 weight_bits = packed & 0xFFFFu;
                                if (weight_bits >= 24u && weight_bits <= 96u) {
                                    const uint32 extra_bits =
                                        partitions == 2u ? 2u : (partitions == 3u ? 5u : 8u);
                                    const uint32 extra_offset = 128u - weight_bits - extra_bits;
                                    uint32 extra = 0u;
                                    for (uint32 bit = 0u; bit < extra_bits; ++bit) {
                                        const uint32 position = extra_offset + bit;
                                        extra |= (((uint32)data[off + (position / 8u)] >>
                                                   (position % 8u)) &
                                                  1u)
                                                 << bit;
                                    }
                                    uint32 cem = ((extra << 6) | base_cem) >> 2;
                                    const uint32 classes = cem;
                                    cem >>= partitions;
                                    for (uint32 i = 0u; i < partitions; ++i) {
                                        const uint32 cls_sub = (((classes >> i) & 1u) != 0) ? 0u : 1u;
                                        const uint32 mode =
                                            ((base_mode - cls_sub) << 2) | ((cem >> (2u * i)) & 3u);
                                        if (((ALPHA_MODES >> mode) & 1u) != 0) {
                                            has_alpha = true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (any(has_alpha)) {
            return false;
        }
    }

    return true;
}
