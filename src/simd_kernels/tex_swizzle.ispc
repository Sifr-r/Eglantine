// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Multi-ISA Tegra block-linear <-> pitch-linear texture swizzle/unswizzle kernel.

static inline uniform uint32 native_gob_offset_u(uniform uint32 offset, uniform bool native_tiling) {
    if (!native_tiling) {
        return offset;
    }
    const uniform uint32 difference = ((offset >> 5) ^ (offset >> 6)) & 1u;
    return offset ^ (difference << 5) ^ (difference << 6);
}

static inline uint32 native_gob_offset_v(uint32 offset, uniform bool native_tiling) {
    if (!native_tiling) {
        return offset;
    }
    const uint32 difference = ((offset >> 5) ^ (offset >> 6)) & 1u;
    return offset ^ (difference << 5) ^ (difference << 6);
}

static inline uniform uint32 pdep_u(uniform uint32 mask, uniform uint32 value) {
    uniform uint32 result = 0u;
    uniform uint32 m = mask;
    for (uniform uint32 bit = 1u; m != 0u; bit += bit) {
        if ((value & bit) != 0u) {
            result |= m & (0u - m);
        }
        m &= m - 1u;
    }
    return result;
}

static inline uint32 pdep_v(uniform uint32 mask, uint32 value) {
    uint32 result = 0u;
    uniform uint32 m = mask;
    for (uniform uint32 bit = 1u; m != 0u; bit += bit) {
        if ((value & bit) != 0u) {
            result |= m & (0u - m);
        }
        m &= m - 1u;
    }
    return result;
}

export void tex_swizzle_copy(uniform uint8 output[], const uniform uint8 input[],
                             uniform uint32 bytes_per_pixel, uniform uint32 width,
                             uniform uint32 height, uniform uint32 depth,
                             uniform uint32 block_height, uniform uint32 block_depth,
                             uniform uint32 stride, uniform bool to_linear,
                             uniform bool native_tiling, uniform bool native_masks) {
    const uniform uint32 GOB_SIZE_X_SHIFT = 6u;
    const uniform uint32 GOB_SIZE_Y_SHIFT = 3u;
    const uniform uint32 GOB_SIZE_SHIFT = 9u;
    const uniform uint32 SWIZZLE_X_BITS = 0x12Fu; // 0b100101111
    const uniform uint32 SWIZZLE_Y_BITS = 0x0D0u; // 0b011010000

    const uniform uint32 x_bits = native_gob_offset_u(SWIZZLE_X_BITS, native_masks);
    const uniform uint32 y_bits = native_gob_offset_u(SWIZZLE_Y_BITS, native_masks);
    const uniform bool apply_native_twist = !native_masks && native_tiling;

    const uniform uint32 pitch = width * bytes_per_pixel;
    const uniform uint32 gobs_in_x = (stride + ((1u << GOB_SIZE_X_SHIFT) - 1u)) >> GOB_SIZE_X_SHIFT;
    const uniform uint32 block_size = gobs_in_x << (GOB_SIZE_SHIFT + block_height + block_depth);
    const uniform uint32 slice_shift = block_height + GOB_SIZE_Y_SHIFT;
    const uniform uint32 slice_size =
        ((height + ((1u << slice_shift) - 1u)) >> slice_shift) * block_size;

    const uniform uint32 block_height_mask = (1u << block_height) - 1u;
    const uniform uint32 block_depth_mask = (1u << block_depth) - 1u;
    const uniform uint32 x_shift = GOB_SIZE_SHIFT + block_height + block_depth;

    for (uniform uint32 z = 0; z < depth; ++z) {
        const uniform uint32 offset_z =
            (z >> block_depth) * slice_size +
            ((z & block_depth_mask) << (GOB_SIZE_SHIFT + block_height));

        for (uniform uint32 y = 0; y < height; ++y) {
            const uniform uint32 swizzled_y = pdep_u(y_bits, y);
            const uniform uint32 block_y = y >> GOB_SIZE_Y_SHIFT;
            const uniform uint32 offset_y = (block_y >> block_height) * block_size +
                                            ((block_y & block_height_mask) << GOB_SIZE_SHIFT);
            const uniform uint32 line_offset = z * pitch * height + y * pitch;
            const uniform uint32 yz_offset = offset_z + offset_y;

            foreach (column = 0 ... width) {
                const uint32 x = column * bytes_per_pixel;
                const uint32 offset_x = (x >> GOB_SIZE_X_SHIFT) << x_shift;
                const uint32 swizzled_x = pdep_v(x_bits, x);
                const uint32 element_offset = yz_offset + offset_x + (swizzled_x | swizzled_y);
                const uint32 swizzled_offset =
                    apply_native_twist ? native_gob_offset_v(element_offset, true) : element_offset;
                const uint32 unswizzled_offset = line_offset + x;

                const uint32 dst_off = to_linear ? swizzled_offset : unswizzled_offset;
                const uint32 src_off = to_linear ? unswizzled_offset : swizzled_offset;

                for (uniform uint32 k = 0; k < bytes_per_pixel; ++k) {
                    output[dst_off + k] = input[src_off + k];
                }
            }
        }
    }
}
