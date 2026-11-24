// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/div_ceil.h"
#include "common/logging.h"
#include "video_core/gpu.h"
#include "video_core/textures/decoders.h"

#ifdef EGLANTINE_ENABLE_ISPC
#include "simd_kernels/tex_swizzle_ispc.h"
#endif

namespace Tegra::Texture {
namespace {
constexpr u32 MaxBlockShift = 15;

[[nodiscard]] constexpr std::optional<u64> CheckedMul(u64 a, u64 b) noexcept {
    if (a != 0 && b > (std::numeric_limits<u64>::max)() / a) {
        return std::nullopt;
    }
    return a * b;
}

[[nodiscard]] constexpr std::optional<u64> CheckedAdd(u64 a, u64 b) noexcept {
    if (b > (std::numeric_limits<u64>::max)() - a) {
        return std::nullopt;
    }
    return a + b;
}

[[nodiscard]] constexpr std::optional<u64> CheckedAlignUpLog2(u64 value, u32 shift) noexcept {
    if (shift >= 64) {
        return std::nullopt;
    }
    const u64 alignment = u64{1} << shift;
    const u64 mask = alignment - 1;
    if (value > (std::numeric_limits<u64>::max)() - mask) {
        return std::nullopt;
    }
    return (value + mask) & ~mask;
}

[[nodiscard]] constexpr std::optional<u64> DivCeilLog2U64(u64 value, u32 shift) noexcept {
    if (shift >= 64) {
        return std::nullopt;
    }
    if (value == 0) {
        return u64{0};
    }
    const u64 low_bits = value & ((u64{1} << shift) - 1);
    return (value >> shift) + (low_bits != 0 ? 1 : 0);
}

struct SwizzleBounds {
    u64 linear_total{};
    u64 max_tiled_end{};
    u64 slice_size{};
};

[[nodiscard]] std::optional<SwizzleBounds> ComputeSwizzleBounds(
    u64 width, u32 height, u32 depth, u32 bytes_per_pixel, u32 block_height, u32 block_depth,
    u64 stride) noexcept {
    if (width == 0 || height == 0 || depth == 0 || bytes_per_pixel == 0 ||
        block_height > MaxBlockShift || block_depth > MaxBlockShift) {
        return std::nullopt;
    }
    const auto pitch = CheckedMul(width, bytes_per_pixel);
    const auto linear_slice = pitch ? CheckedMul(*pitch, height) : std::nullopt;
    const auto linear_total = linear_slice ? CheckedMul(*linear_slice, depth) : std::nullopt;
    if (!linear_total) {
        return std::nullopt;
    }

    const u32 block_shift = GOB_SIZE_SHIFT + block_height + block_depth;
    if (block_shift >= 64) {
        return std::nullopt;
    }
    const auto gobs_in_x = DivCeilLog2U64(stride, GOB_SIZE_X_SHIFT);
    const auto block_size = gobs_in_x ? CheckedMul(*gobs_in_x, u64{1} << block_shift) : std::nullopt;
    const auto gobs_in_y = DivCeilLog2U64(height, block_height + GOB_SIZE_Y_SHIFT);
    const auto slice_size =
        (block_size && gobs_in_y) ? CheckedMul(*gobs_in_y, *block_size) : std::nullopt;
    if (!slice_size) {
        return std::nullopt;
    }

    const u64 block_height_mask = (u64{1} << block_height) - 1;
    const u64 block_depth_mask = (u64{1} << block_depth) - 1;

    const u64 max_z = static_cast<u64>(depth) - 1;
    const auto z_hi = CheckedMul(max_z >> block_depth, *slice_size);
    const u64 z_lo = (max_z & block_depth_mask) << (GOB_SIZE_SHIFT + block_height);
    const auto max_offset_z = z_hi ? CheckedAdd(*z_hi, z_lo) : std::nullopt;

    const u64 max_y = static_cast<u64>(height) - 1;
    const u64 max_block_y = max_y >> GOB_SIZE_Y_SHIFT;
    const auto y_hi = CheckedMul(max_block_y >> block_height, *block_size);
    const u64 y_lo = (max_block_y & block_height_mask) << GOB_SIZE_SHIFT;
    const auto max_offset_y = y_hi ? CheckedAdd(*y_hi, y_lo) : std::nullopt;

    const u64 max_x = *pitch - bytes_per_pixel;
    const auto max_offset_x = CheckedMul(max_x >> GOB_SIZE_X_SHIFT, u64{1} << block_shift);

    if (!max_offset_z || !max_offset_y || !max_offset_x) {
        return std::nullopt;
    }
    const auto zy = CheckedAdd(*max_offset_z, *max_offset_y);
    const auto zyx = zy ? CheckedAdd(*zy, *max_offset_x) : std::nullopt;
    const u64 gob_tail =
        (bytes_per_pixel <= 16 && 16 % bytes_per_pixel == 0) ? GOB_SIZE : GOB_SIZE + bytes_per_pixel;
    const auto max_tiled_end = zyx ? CheckedAdd(*zyx, gob_tail) : std::nullopt;
    if (!max_tiled_end) {
        return std::nullopt;
    }
    return SwizzleBounds{
        .linear_total = *linear_total,
        .max_tiled_end = *max_tiled_end,
        .slice_size = *slice_size,
    };
}

constexpr u32 NativeGobOffset(u32 offset, bool native_tiling) {
    if (!native_tiling) return offset;
    const u32 difference = ((offset >> 5) ^ (offset >> 6)) & 1;
    return offset ^ (difference << 5) ^ (difference << 6);
}
template <u32 mask>
constexpr u32 pdep(u32 value) {
    u32 result = 0;
    u32 m = mask;
    for (u32 bit = 1; m; bit += bit) {
        if (value & bit)
            result |= m & (~m + 1);
        m &= m - 1;
    }
    return result;
}

template <u32 mask, u32 incr_amount>
void incrpdep(u32& value) {
    static constexpr u32 swizzled_incr = pdep<mask>(incr_amount);
    value = ((value | ~mask) + swizzled_incr) & mask;
}

template <bool TO_LINEAR, u32 BYTES_PER_PIXEL, bool NATIVE_MASKS = false>
void SwizzleImpl(std::span<u8> output, std::span<const u8> input, u64 width, u32 height, u32 depth,
                 u32 block_height, u32 block_depth, u64 stride, bool native_tiling) {
    if (output.empty() || input.empty()) {
        return;
    }
    const auto bounds =
        ComputeSwizzleBounds(width, height, depth, BYTES_PER_PIXEL, block_height, block_depth, stride);
    if (!bounds) {
        return;
    }
    const size_t tiled_size = TO_LINEAR ? output.size() : input.size();
    const size_t linear_size = TO_LINEAR ? input.size() : output.size();
    if (bounds->max_tiled_end > tiled_size || bounds->linear_total > linear_size) {
        return;
    }

    // Native GOBs exchange bits 5 and 6; deposit them directly instead of swapping each element.
    static constexpr u32 x_bits = NativeGobOffset(SWIZZLE_X_BITS, NATIVE_MASKS);
    static constexpr u32 y_bits = NativeGobOffset(SWIZZLE_Y_BITS, NATIVE_MASKS);
    // The origin of the transformation can be configured here, leave it as zero as the current API
    // doesn't expose it.
    static constexpr u32 origin_x = 0;
    static constexpr u32 origin_y = 0;
    static constexpr u32 origin_z = 0;

    // We can configure here a custom pitch
    // As it's not exposed 'width * BYTES_PER_PIXEL' will be the expected pitch.
    const u64 pitch = width * BYTES_PER_PIXEL;

    const u64 gobs_in_x = *DivCeilLog2U64(stride, GOB_SIZE_X_SHIFT);
    const u32 x_shift = GOB_SIZE_SHIFT + block_height + block_depth;
    const u64 block_size = gobs_in_x << x_shift;
    const u64 slice_size = bounds->slice_size;

    const u64 block_height_mask = (u64{1} << block_height) - 1;
    const u64 block_depth_mask = (u64{1} << block_depth) - 1;

    // Both swizzle masks and the native GOB twist only touch bits 4 and up of the byte offset, so
    // a 16-byte run that starts on a 16-byte boundary in one layout stays one 16-byte run in the
    // other. Copy whole chunks instead of one pixel at a time: a 4-bytes-per-pixel tile drops from
    // four offset computations and four 4-byte copies to one of each per 16 bytes.
    constexpr u32 CHUNK_BYTES = 16;
    constexpr bool CAN_CHUNK = BYTES_PER_PIXEL <= CHUNK_BYTES && CHUNK_BYTES % BYTES_PER_PIXEL == 0;
    constexpr u64 CHUNK_COLUMNS = CAN_CHUNK ? CHUNK_BYTES / BYTES_PER_PIXEL : 1;

    for (u32 slice = 0; slice < depth; ++slice) {
        const u64 z = static_cast<u64>(slice) + origin_z;
        const u64 offset_z = (z >> block_depth) * slice_size +
                             ((z & block_depth_mask) << (GOB_SIZE_SHIFT + block_height));
        for (u32 line = 0; line < height; ++line) {
            const u32 y = line + origin_y;
            const u32 swizzled_y = pdep<y_bits>(y);

            const u64 block_y = y >> GOB_SIZE_Y_SHIFT;
            const u64 offset_y = (block_y >> block_height) * block_size +
                                 ((block_y & block_height_mask) << GOB_SIZE_SHIFT);
            const u64 line_offset =
                (static_cast<u64>(slice) * height + static_cast<u64>(line)) * pitch;

            u32 swizzled_x = pdep<x_bits>(origin_x * BYTES_PER_PIXEL);
            u64 column = 0;
            if constexpr (CAN_CHUNK) {
                const u64 chunk_end = width - width % CHUNK_COLUMNS;
                for (; column < chunk_end; column += CHUNK_COLUMNS) {
                    const u64 x = (column + origin_x) * BYTES_PER_PIXEL;
                    const u64 offset_x = (x >> GOB_SIZE_X_SHIFT) << x_shift;

                    const u64 base_swizzled_offset = offset_z + offset_y + offset_x;
                    const u32 element_offset = swizzled_x | swizzled_y;
                    const u64 swizzled_offset =
                        base_swizzled_offset +
                        (NATIVE_MASKS ? element_offset
                                      : NativeGobOffset(element_offset, native_tiling));

                    const u64 unswizzled_offset = line_offset + column * BYTES_PER_PIXEL;

                    u8* const dst = &output[TO_LINEAR ? swizzled_offset : unswizzled_offset];
                    const u8* const src = &input[TO_LINEAR ? unswizzled_offset : swizzled_offset];
                    std::memcpy(dst, src, CHUNK_BYTES);
                    // The same advance CHUNK_COLUMNS pixel steps make: the masked add carries
                    // through the permuted bits like the per-pixel increments do.
                    incrpdep<x_bits, CHUNK_BYTES>(swizzled_x);
                }
            }
            for (; column < width; ++column, incrpdep<x_bits, BYTES_PER_PIXEL>(swizzled_x)) {
                const u64 x = (column + origin_x) * BYTES_PER_PIXEL;
                const u64 offset_x = (x >> GOB_SIZE_X_SHIFT) << x_shift;

                const u64 base_swizzled_offset = offset_z + offset_y + offset_x;
                const u32 element_offset = swizzled_x | swizzled_y;
                const u64 swizzled_offset =
                    base_swizzled_offset +
                    (NATIVE_MASKS ? element_offset
                                  : NativeGobOffset(element_offset, native_tiling));

                const u64 unswizzled_offset = line_offset + column * BYTES_PER_PIXEL;

                u8* const dst = &output[TO_LINEAR ? swizzled_offset : unswizzled_offset];
                const u8* const src = &input[TO_LINEAR ? unswizzled_offset : swizzled_offset];

                std::memcpy(dst, src, BYTES_PER_PIXEL);
            }
        }
    }
}

template <bool TO_LINEAR, u32 BYTES_PER_PIXEL>
void SwizzleSubrectImpl(std::span<u8> output, std::span<const u8> input, u32 width, u32 height,
                        u32 depth, u32 origin_x, u32 origin_y, u32 extent_x, u32 num_lines,
                        u32 block_height, u32 block_depth, u32 pitch_linear, bool native_tiling) {
    if (width == 0 || height == 0 || depth == 0 || extent_x == 0 || num_lines == 0 ||
        origin_x >= width || origin_y >= height || block_height > MaxBlockShift ||
        block_depth > MaxBlockShift || output.empty() || input.empty()) {
        return;
    }
    extent_x = (std::min)(extent_x, width - origin_x);

    // The origin of the transformation can be configured here, leave it as zero as the current API
    // doesn't expose it.
    static constexpr u32 origin_z = 0;

    // We can configure here a custom pitch
    // As it's not exposed 'width * BYTES_PER_PIXEL' will be the expected pitch.
    const u64 pitch = pitch_linear;
    const auto row_bytes = CheckedMul(width, BYTES_PER_PIXEL);
    const auto stride = row_bytes ? CheckedAlignUpLog2(*row_bytes, GOB_SIZE_X_SHIFT) : std::nullopt;
    const auto gobs_in_x = stride ? DivCeilLog2U64(*stride, GOB_SIZE_X_SHIFT) : std::nullopt;
    const u32 x_shift = GOB_SIZE_SHIFT + block_height + block_depth;
    const auto block_size =
        gobs_in_x ? CheckedMul(*gobs_in_x, u64{1} << x_shift) : std::nullopt;
    const auto gobs_in_y = DivCeilLog2U64(height, block_height + GOB_SIZE_Y_SHIFT);
    const auto slice_size =
        (block_size && gobs_in_y) ? CheckedMul(*gobs_in_y, *block_size) : std::nullopt;
    const auto linear_slice_bytes = CheckedMul(pitch, height);
    const u64 max_x = static_cast<u64>(origin_x + extent_x - 1) * BYTES_PER_PIXEL;
    const auto max_offset_x = CheckedMul(max_x >> GOB_SIZE_X_SHIFT, u64{1} << x_shift);
    if (!slice_size || !linear_slice_bytes || !max_offset_x) {
        return;
    }

    const u64 block_height_mask = (u64{1} << block_height) - 1;
    const u64 block_depth_mask = (u64{1} << block_depth) - 1;
    const size_t tiled_size = TO_LINEAR ? output.size() : input.size();
    const size_t linear_size = TO_LINEAR ? input.size() : output.size();
    const u64 extent_bytes = static_cast<u64>(extent_x) * BYTES_PER_PIXEL;
    if (BYTES_PER_PIXEL > tiled_size || extent_bytes > linear_size) {
        return;
    }

    u32 unprocessed_lines = num_lines;
    const u32 extent_y = (std::min)(num_lines, height - origin_y);

    for (u32 slice = 0; slice < depth; ++slice) {
        const u64 z = static_cast<u64>(slice) + origin_z;
        const auto offset_z = CheckedMul(z >> block_depth, *slice_size);
        const auto slice_linear_base = CheckedMul(slice, *linear_slice_bytes);
        if (!offset_z || !slice_linear_base) {
            return;
        }
        const u64 z_gob_offset =
            *offset_z + ((z & block_depth_mask) << (GOB_SIZE_SHIFT + block_height));
        const u32 lines_in_y = (std::min)(unprocessed_lines, extent_y);
        for (u32 line = 0; line < lines_in_y; ++line) {
            const u64 line_unswizzled = *slice_linear_base + static_cast<u64>(line) * pitch;
            if (line_unswizzled > linear_size || extent_bytes > linear_size - line_unswizzled) {
                return;
            }
            const u32 y = line + origin_y;
            const u32 swizzled_y = pdep<SWIZZLE_Y_BITS>(y);

            const u64 block_y = y >> GOB_SIZE_Y_SHIFT;
            const u64 offset_y = (block_y >> block_height) * (*block_size) +
                                 ((block_y & block_height_mask) << GOB_SIZE_SHIFT);

            u32 swizzled_x = pdep<SWIZZLE_X_BITS>(origin_x * BYTES_PER_PIXEL);
            for (u32 column = 0; column < extent_x;
                 ++column, incrpdep<SWIZZLE_X_BITS, BYTES_PER_PIXEL>(swizzled_x)) {
                const u64 x = static_cast<u64>(column + origin_x) * BYTES_PER_PIXEL;
                const u64 offset_x = (x >> GOB_SIZE_X_SHIFT) << x_shift;

                const u64 base_swizzled_offset = z_gob_offset + offset_y + offset_x;
                const u64 swizzled_offset =
                    base_swizzled_offset +
                    NativeGobOffset(swizzled_x | swizzled_y, native_tiling);
                if (swizzled_offset > tiled_size - BYTES_PER_PIXEL) {
                    continue;
                }

                const u64 unswizzled_offset =
                    line_unswizzled + static_cast<u64>(column) * BYTES_PER_PIXEL;

                u8* const dst = &output[TO_LINEAR ? swizzled_offset : unswizzled_offset];
                const u8* const src = &input[TO_LINEAR ? unswizzled_offset : swizzled_offset];

                std::memcpy(dst, src, BYTES_PER_PIXEL);
            }
        }
        unprocessed_lines -= lines_in_y;
        if (unprocessed_lines == 0) {
            return;
        }
    }
}

#ifdef EGLANTINE_ENABLE_ISPC
bool IspcEnvEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_ISPC");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

bool IspcVerifyEnabled() noexcept {
    static const bool verify = [] {
        const char* value = std::getenv("EDEN_NX2_ISPC_VERIFY");
        return value != nullptr && value[0] == '1';
    }();
    return verify;
}
#endif

template <bool TO_LINEAR>
void SwizzleScalar(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u64 width,
                   u32 height, u32 depth, u32 block_height, u32 block_depth, u64 stride,
                   bool native_tiling, bool native_masks) {
    switch (bytes_per_pixel) {
#define BPP_CASE(x)                                                                                \
    case x:                                                                                        \
        if (native_tiling && native_masks) {                                                       \
            return SwizzleImpl<TO_LINEAR, x, true>(output, input, width, height, depth,            \
                                                   block_height, block_depth, stride, true);       \
        }                                                                                          \
        return SwizzleImpl<TO_LINEAR, x>(output, input, width, height, depth, block_height,        \
                                         block_depth, stride, native_tiling);
        BPP_CASE(1)
        BPP_CASE(2)
        BPP_CASE(3)
        BPP_CASE(4)
        BPP_CASE(6)
        BPP_CASE(8)
        BPP_CASE(12)
        BPP_CASE(16)
#undef BPP_CASE
    default:
        ASSERT_MSG(false, "Invalid bytes_per_pixel={}", bytes_per_pixel);
        break;
    }
}

template <bool TO_LINEAR>
void Swizzle(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u64 width,
             u32 height, u32 depth, u32 block_height, u32 block_depth, u64 stride,
             bool native_tiling) {
    static const bool native_masks = [] {
        const char* value = std::getenv("EDEN_NX2_NATIVE_SWIZZLE_MASKS");
        return value != nullptr && value[0] == '1';
    }();
    if (output.empty() || input.empty()) {
        return;
    }
    const auto bounds =
        ComputeSwizzleBounds(width, height, depth, bytes_per_pixel, block_height, block_depth, stride);
    if (!bounds) {
        return;
    }
    const size_t tiled_size = TO_LINEAR ? output.size() : input.size();
    const size_t linear_size = TO_LINEAR ? input.size() : output.size();
    if (bounds->max_tiled_end > tiled_size || bounds->linear_total > linear_size) {
        return;
    }
#ifdef EGLANTINE_ENABLE_ISPC
    constexpr u64 ispc_max = static_cast<u64>((std::numeric_limits<s32>::max)());
    const bool ispc_safe = IspcEnvEnabled() &&
                           (GOB_SIZE_SHIFT + block_height + block_depth) < 31 &&
                           width <= ispc_max && stride <= ispc_max &&
                           bounds->linear_total <= ispc_max && bounds->max_tiled_end <= ispc_max &&
                           bounds->slice_size <= ispc_max;
    if (ispc_safe) {
        const u32 width_u32 = static_cast<u32>(width);
        const u32 stride_u32 = static_cast<u32>(stride);
        if (IspcVerifyEnabled()) {
            std::vector<u8> scalar_out(output.begin(), output.end());
            SwizzleScalar<TO_LINEAR>(scalar_out, input, bytes_per_pixel, width, height, depth,
                                     block_height, block_depth, stride, native_tiling,
                                     native_masks);
            ispc::tex_swizzle_copy(output.data(), input.data(), bytes_per_pixel, width_u32, height,
                                   depth, block_height, block_depth, stride_u32, TO_LINEAR,
                                   native_tiling, native_tiling && native_masks);
            if (std::memcmp(output.data(), scalar_out.data(), output.size()) != 0) {
                LOG_ERROR(HW_GPU,
                          "EDEN_NX2_ISPC_VERIFY: swizzle mismatch (to_linear={}, bpp={}, {}x{}x{})",
                          TO_LINEAR, bytes_per_pixel, width, height, depth);
                std::memcpy(output.data(), scalar_out.data(), output.size());
            }
            return;
        }
        ispc::tex_swizzle_copy(output.data(), input.data(), bytes_per_pixel, width_u32, height,
                               depth, block_height, block_depth, stride_u32, TO_LINEAR,
                               native_tiling, native_tiling && native_masks);
        return;
    }
#endif
    SwizzleScalar<TO_LINEAR>(output, input, bytes_per_pixel, width, height, depth, block_height,
                             block_depth, stride, native_tiling, native_masks);
}

} // Anonymous namespace

bool IsIspcEnabled() noexcept {
#ifdef EGLANTINE_ENABLE_ISPC
    return IspcEnvEnabled();
#else
    return false;
#endif
}

void UnswizzleTexture(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                      u32 width, u32 height, u32 depth, u32 block_height, u32 block_depth,
                      u32 stride_alignment, bool native_tiling) {
    if (bytes_per_pixel == 0 || width == 0 || height == 0 || depth == 0 ||
        stride_alignment >= 32) {
        return;
    }
    const u64 aligned_width = Common::AlignUpLog2(static_cast<u64>(width), stride_alignment);
    const u64 stride = aligned_width * bytes_per_pixel;
    const u64 row_bytes = static_cast<u64>(width) * bytes_per_pixel;
    const u32 new_bpp = (std::min)(4U, static_cast<u32>(std::countr_zero(row_bytes)));
    const u64 new_width = row_bytes >> new_bpp;
    const u32 packed_bpp = 1U << new_bpp;
    Swizzle<false>(output, input, packed_bpp, new_width, height, depth, block_height, block_depth,
                   stride, native_tiling);
}

void SwizzleTexture(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
                    u32 height, u32 depth, u32 block_height, u32 block_depth,
                    u32 stride_alignment, bool native_tiling) {
    if (bytes_per_pixel == 0 || width == 0 || height == 0 || depth == 0 ||
        stride_alignment >= 32) {
        return;
    }
    const u64 aligned_width = Common::AlignUpLog2(static_cast<u64>(width), stride_alignment);
    const u64 stride = aligned_width * bytes_per_pixel;
    const u64 row_bytes = static_cast<u64>(width) * bytes_per_pixel;
    const u32 new_bpp = (std::min)(4U, static_cast<u32>(std::countr_zero(row_bytes)));
    const u64 new_width = row_bytes >> new_bpp;
    const u32 packed_bpp = 1U << new_bpp;
    Swizzle<true>(output, input, packed_bpp, new_width, height, depth, block_height, block_depth,
                  stride, native_tiling);
}

void SwizzleSubrect(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
                    u32 height, u32 depth, u32 origin_x, u32 origin_y, u32 extent_x, u32 extent_y,
                    u32 block_height, u32 block_depth, u32 pitch_linear, bool native_tiling) {
    switch (bytes_per_pixel) {
#define BPP_CASE(x)                                                                                \
    case x:                                                                                        \
        return SwizzleSubrectImpl<true, x>(output, input, width, height, depth, origin_x,          \
                                           origin_y, extent_x, extent_y, block_height,             \
                                           block_depth, pitch_linear, native_tiling);
        BPP_CASE(1)
        BPP_CASE(2)
        BPP_CASE(3)
        BPP_CASE(4)
        BPP_CASE(6)
        BPP_CASE(8)
        BPP_CASE(12)
        BPP_CASE(16)
#undef BPP_CASE
    default:
        ASSERT_MSG(false, "Invalid bytes_per_pixel={}", bytes_per_pixel);
        break;
    }
}

void UnswizzleSubrect(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                      u32 width, u32 height, u32 depth, u32 origin_x, u32 origin_y, u32 extent_x,
                      u32 extent_y, u32 block_height, u32 block_depth, u32 pitch_linear, bool native_tiling) {
    switch (bytes_per_pixel) {
#define BPP_CASE(x)                                                                                \
    case x:                                                                                        \
        return SwizzleSubrectImpl<false, x>(output, input, width, height, depth, origin_x,         \
                                            origin_y, extent_x, extent_y, block_height,            \
                                            block_depth, pitch_linear, native_tiling);
        BPP_CASE(1)
        BPP_CASE(2)
        BPP_CASE(3)
        BPP_CASE(4)
        BPP_CASE(6)
        BPP_CASE(8)
        BPP_CASE(12)
        BPP_CASE(16)
#undef BPP_CASE
    default:
        ASSERT_MSG(false, "Invalid bytes_per_pixel={}", bytes_per_pixel);
        break;
    }
}

std::size_t CalculateSize(bool tiled, u32 bytes_per_pixel, u32 width, u32 height, u32 depth,
                          u32 block_height, u32 block_depth) {
    if (bytes_per_pixel == 0 || width == 0 || height == 0 || depth == 0) {
        return 0;
    }
    if (tiled) {
        if (block_height > MaxBlockShift || block_depth > MaxBlockShift) {
            return 0;
        }
        const auto row_bytes = CheckedMul(width, bytes_per_pixel);
        if (!row_bytes) {
            return 0;
        }
        const auto aligned_width = CheckedAlignUpLog2(*row_bytes, GOB_SIZE_X_SHIFT);
        const auto aligned_height = CheckedAlignUpLog2(height, GOB_SIZE_Y_SHIFT + block_height);
        const auto aligned_depth = CheckedAlignUpLog2(depth, GOB_SIZE_Z_SHIFT + block_depth);
        if (!aligned_width || !aligned_height || !aligned_depth) {
            return 0;
        }
        const auto slice = CheckedMul(*aligned_width, *aligned_height);
        if (!slice) {
            return 0;
        }
        const auto total = CheckedMul(*slice, *aligned_depth);
        if (!total || *total > (std::numeric_limits<std::size_t>::max)()) {
            return 0;
        }
        return static_cast<std::size_t>(*total);
    } else {
        const auto wh = CheckedMul(width, height);
        const auto whd = wh ? CheckedMul(*wh, depth) : std::nullopt;
        const auto total = whd ? CheckedMul(*whd, bytes_per_pixel) : std::nullopt;
        if (!total || *total > (std::numeric_limits<std::size_t>::max)()) {
            return 0;
        }
        return static_cast<std::size_t>(*total);
    }
}

u64 GetGOBOffset(u32 width, u32 height, u32 dst_x, u32 dst_y, u32 block_height,
                 u32 bytes_per_pixel) {
    if (bytes_per_pixel == 0 || bytes_per_pixel > GOB_SIZE_X || block_height > MaxBlockShift) {
        return 0;
    }
    const u64 gobs_in_block = u64{1} << block_height;
    const u64 y_blocks = static_cast<u64>(GOB_SIZE_Y) << block_height;
    const u64 x_per_gob = GOB_SIZE_X / bytes_per_pixel;
    const u64 x_blocks = (static_cast<u64>(width) + x_per_gob - 1) / x_per_gob;
    const u64 block_size = GOB_SIZE * gobs_in_block;
    const u64 stride = block_size * x_blocks;
    const u64 base =
        (static_cast<u64>(dst_y) / y_blocks) * stride + (static_cast<u64>(dst_x) / x_per_gob) * block_size;
    const u64 relative_y = static_cast<u64>(dst_y) % y_blocks;
    return base + (relative_y / GOB_SIZE_Y) * GOB_SIZE;
}

} // namespace Tegra::Texture
