// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <set>
#include <vector>

#include "common/alignment.h"
#include "common/literals.h"
#include "common/scratch_buffer.h"
#include "video_core/engines/sw_blitter/blitter.h"
#include "video_core/engines/sw_blitter/converter.h"
#include "video_core/guest_memory.h"
#include "video_core/memory_manager.h"
#include "video_core/surface.h"
#include "video_core/textures/decoders.h"

namespace Tegra {
class MemoryManager;
}

using VideoCore::Surface::BytesPerBlock;
using VideoCore::Surface::PixelFormatFromRenderTargetFormat;

namespace Tegra::Engines::Blitter {

using namespace Texture;
using namespace Common::Literals;

namespace {

constexpr size_t ir_components = 4;

void NearestNeighbor(std::span<const u8> input, std::span<u8> output, u32 src_width, u32 src_height,
                     u32 dst_width, u32 dst_height, size_t bpp) {
    if (src_width == 0 || src_height == 0 || dst_width == 0 || dst_height == 0 || bpp == 0) {
        return;
    }
    const size_t max_src_row = static_cast<size_t>(src_height - 1);
    const size_t max_src_col = static_cast<size_t>(src_width - 1);
    const u64 dx_du =
        static_cast<u64>(std::llround((static_cast<f64>(src_width) / dst_width) * (1ULL << 32)));
    const u64 dy_dv =
        static_cast<u64>(std::llround((static_cast<f64>(src_height) / dst_height) * (1ULL << 32)));
    u64 src_y = 0;
    for (u32 y = 0; y < dst_height; y++) {
        const size_t sy = (std::min)(static_cast<size_t>(src_y >> 32), max_src_row);
        u64 src_x = 0;
        for (u32 x = 0; x < dst_width; x++) {
            const size_t sx = (std::min)(static_cast<size_t>(src_x >> 32), max_src_col);
            const size_t read_from = (sy * src_width + sx) * bpp;
            const size_t write_to = (static_cast<size_t>(y) * dst_width + x) * bpp;

            std::memcpy(&output[write_to], &input[read_from], bpp);
            src_x += dx_du;
        }
        src_y += dy_dv;
    }
}

void NearestNeighborFast(std::span<const f32> input, std::span<f32> output, u32 src_width,
                         u32 src_height, u32 dst_width, u32 dst_height) {
    if (src_width == 0 || src_height == 0 || dst_width == 0 || dst_height == 0) {
        return;
    }
    const size_t max_src_row = static_cast<size_t>(src_height - 1);
    const size_t max_src_col = static_cast<size_t>(src_width - 1);
    const u64 dx_du =
        static_cast<u64>(std::llround((static_cast<f64>(src_width) / dst_width) * (1ULL << 32)));
    const u64 dy_dv =
        static_cast<u64>(std::llround((static_cast<f64>(src_height) / dst_height) * (1ULL << 32)));
    u64 src_y = 0;
    for (u32 y = 0; y < dst_height; y++) {
        const size_t sy = (std::min)(static_cast<size_t>(src_y >> 32), max_src_row);
        u64 src_x = 0;
        for (u32 x = 0; x < dst_width; x++) {
            const size_t sx = (std::min)(static_cast<size_t>(src_x >> 32), max_src_col);
            const size_t read_from = (sy * src_width + sx) * ir_components;
            const size_t write_to = (static_cast<size_t>(y) * dst_width + x) * ir_components;

            std::memcpy(&output[write_to], &input[read_from], sizeof(f32) * ir_components);
            src_x += dx_du;
        }
        src_y += dy_dv;
    }
}

void Bilinear(std::span<const f32> input, std::span<f32> output, size_t src_width,
              size_t src_height, size_t dst_width, size_t dst_height) {
    if (src_width == 0 || src_height == 0 || dst_width == 0 || dst_height == 0) {
        return;
    }
    const auto bilinear_sample = [](std::span<const f32> x0_y0, std::span<const f32> x1_y0,
                                    std::span<const f32> x0_y1, std::span<const f32> x1_y1,
                                    f32 weight_x, f32 weight_y) {
        std::array<f32, ir_components> result{};
        for (size_t i = 0; i < ir_components; i++) {
            const f32 a = std::lerp(x0_y0[i], x1_y0[i], weight_x);
            const f32 b = std::lerp(x0_y1[i], x1_y1[i], weight_x);
            result[i] = std::lerp(a, b, weight_y);
        }
        return result;
    };
    const f32 dx_du =
        dst_width > 1 ? static_cast<f32>(src_width - 1) / static_cast<f32>(dst_width - 1) : 0.f;
    const f32 dy_dv =
        dst_height > 1 ? static_cast<f32>(src_height - 1) / static_cast<f32>(dst_height - 1) : 0.f;
    for (size_t y = 0; y < dst_height; y++) {
        for (size_t x = 0; x < dst_width; x++) {
            const f32 x_low = std::floor(static_cast<f32>(x) * dx_du);
            const f32 y_low = std::floor(static_cast<f32>(y) * dy_dv);
            const f32 x_high = std::ceil(static_cast<f32>(x) * dx_du);
            const f32 y_high = std::ceil(static_cast<f32>(y) * dy_dv);
            const f32 weight_x = (static_cast<f32>(x) * dx_du) - x_low;
            const f32 weight_y = (static_cast<f32>(y) * dy_dv) - y_low;

            const auto read_src = [&](f32 in_x, f32 in_y) {
                const size_t ix =
                    (std::min)(in_x > 0.0f ? static_cast<size_t>(in_x) : size_t{0}, src_width - 1);
                const size_t iy = (std::min)(
                    in_y > 0.0f ? static_cast<size_t>(in_y) : size_t{0}, src_height - 1);
                const size_t read_from = (iy * src_width + ix) * ir_components;
                return std::span<const f32>(&input[read_from], ir_components);
            };

            auto x0_y0 = read_src(x_low, y_low);
            auto x1_y0 = read_src(x_high, y_low);
            auto x0_y1 = read_src(x_low, y_high);
            auto x1_y1 = read_src(x_high, y_high);

            const auto result = bilinear_sample(x0_y0, x1_y0, x0_y1, x1_y1, weight_x, weight_y);

            const size_t write_to = (y * dst_width + x) * ir_components;

            std::memcpy(&output[write_to], &result, sizeof(f32) * ir_components);
        }
    }
}

template <bool unpack>
void ProcessPitchLinear(std::span<const u8> input, std::span<u8> output, size_t extent_x,
                        size_t extent_y, u32 pitch, u32 x0, u32 y0, size_t bpp) {
    const size_t base_offset = x0 * bpp;
    const size_t copy_size = extent_x * bpp;
    for (size_t y = 0; y < extent_y; y++) {
        const size_t first_offset = (y + y0) * pitch + base_offset;
        const size_t second_offset = y * extent_x * bpp;
        u8* write_to = unpack ? &output[first_offset] : &output[second_offset];
        const u8* read_from = unpack ? &input[second_offset] : &input[first_offset];
        std::memcpy(write_to, read_from, copy_size);
    }
}

struct SubSurface {
    u64 offset;    ///< From the surface's address
    size_t size;   ///< Bytes
    u32 height;    ///< Lines the part has
    u32 origin_y;  ///< The blit's first line in it
};

// The part of a surface a blit covers: the rows of blocks (block linear) or the lines (pitch) from
// its first line to its last. A tile copied into a layer of a large texture used to read and write
// back the whole layer (64-256 MiB), which also gave every block of a Switch 2 sparse layer memory.
SubSurface CoveredRows(const Fermi2D::Surface& surface, u32 bytes_per_pixel, s32 x0, s32 y0,
                       u32 extent_x, u32 extent_y, size_t surface_size) {
    const u32 safe_y0 = y0 >= 0 ? static_cast<u32>(y0) : 0U;
    const SubSurface whole{0, surface_size, surface.height, safe_y0};
    if (x0 < 0 || y0 < 0 || extent_x == 0 || extent_y == 0 || bytes_per_pixel == 0 ||
        surface_size == 0) {
        return whole;
    }
    const u64 first_line = static_cast<u32>(y0);
    if (surface.linear == Fermi2D::MemoryLayout::BlockLinear) {
        const u32 block_height = surface.block_height;
        const u32 block_depth = surface.block_depth;
        if (surface.depth != 1 || block_height > 15 || block_depth > 15) {
            return whole;
        }
        const u32 lines_shift = GOB_SIZE_Y_SHIFT + block_height;
        const u32 total_shift = lines_shift + block_depth;
        const u64 gob_row_bytes = static_cast<u64>(surface.width) * bytes_per_pixel;
        const u64 aligned_row_bytes = Common::AlignUpLog2(gob_row_bytes, GOB_SIZE_X_SHIFT);
        if (total_shift >= 64 || aligned_row_bytes > ((std::numeric_limits<u64>::max)() >> total_shift)) {
            return whole;
        }
        const u64 row_size = aligned_row_bytes << total_shift;
        const u64 first_row = first_line >> lines_shift;
        const u64 last_line = first_line + static_cast<u64>(extent_y - 1);
        const u64 rows = (last_line >> lines_shift) - first_row + 1;
        if (row_size == 0 || first_row > (std::numeric_limits<u64>::max)() / row_size ||
            rows > (std::numeric_limits<u64>::max)() / row_size ||
            rows > ((std::numeric_limits<u32>::max)() >> lines_shift)) {
            return whole;
        }
        const u64 offset = first_row * row_size;
        const u64 part_size = rows * row_size;
        if (part_size > (std::numeric_limits<size_t>::max)() || offset > surface_size ||
            part_size > surface_size - offset) {
            return whole;
        }
        return SubSurface{offset, static_cast<size_t>(part_size),
                          static_cast<u32>(rows << lines_shift),
                          static_cast<u32>(first_line - (first_row << lines_shift))};
    }
    const u64 pitch = surface.pitch;
    const u64 offset = first_line * pitch;
    const u64 row_span = (static_cast<u64>(x0) + extent_x) * bytes_per_pixel;
    const u64 rows_bytes = static_cast<u64>(extent_y - 1) * pitch;
    if (rows_bytes > (std::numeric_limits<u64>::max)() - row_span) {
        return whole;
    }
    const u64 part_size = rows_bytes + row_span;
    if (part_size > (std::numeric_limits<size_t>::max)() || offset > surface_size ||
        part_size > surface_size - offset) {
        return whole;
    }
    return SubSurface{offset, static_cast<size_t>(part_size), extent_y, 0};
}

} // namespace

struct SoftwareBlitEngine::BlitEngineImpl {
    Common::ScratchBuffer<u8> tmp_buffer;
    Common::ScratchBuffer<u8> src_buffer;
    Common::ScratchBuffer<u8> dst_buffer;
    Common::ScratchBuffer<f32> intermediate_src;
    Common::ScratchBuffer<f32> intermediate_dst;

    // Buffers grown for one large surface are freed after the blit rather than kept for the rest
    // of the session; small ones stay. 16 MiB kept title 1's boot mip blits (64/16/4/1 MiB levels)
    // in 16 such buffers, 256 MiB, across its 2D engines.
    void ReleaseLargeBuffers() {
        constexpr size_t LargeCapacity = 2_MiB;
        for (auto* const buffer : {&tmp_buffer, &src_buffer, &dst_buffer}) {
            if (buffer->capacity() > LargeCapacity) {
                buffer->release();
            }
        }
        for (auto* const buffer : {&intermediate_src, &intermediate_dst}) {
            if (buffer->capacity() * sizeof(f32) > LargeCapacity) {
                buffer->release();
            }
        }
    }
    ConverterFactory converter_factory;
};

SoftwareBlitEngine::SoftwareBlitEngine(MemoryManager& memory_manager_)
    : memory_manager{memory_manager_} {
    impl = std::make_unique<BlitEngineImpl>();
}

SoftwareBlitEngine::~SoftwareBlitEngine() = default;

bool SoftwareBlitEngine::Blit(Fermi2D::Surface& src, Fermi2D::Surface& dst,
                              Fermi2D::Config& config) {
    if (config.src_x0 < 0 || config.src_y0 < 0 || config.src_x1 <= config.src_x0 ||
        config.src_y1 <= config.src_y0 || config.dst_x0 < 0 || config.dst_y0 < 0 ||
        config.dst_x1 <= config.dst_x0 || config.dst_y1 <= config.dst_y0) {
        return false;
    }
    if (src.width == 0 || src.height == 0 || src.depth == 0 || dst.width == 0 ||
        dst.height == 0 || dst.depth == 0) {
        return false;
    }
    if (static_cast<u32>(config.src_x1) > src.width ||
        static_cast<u32>(config.src_y1) > src.height ||
        static_cast<u32>(config.dst_x1) > dst.width ||
        static_cast<u32>(config.dst_y1) > dst.height) {
        return false;
    }

    const auto get_surface_size = [](Fermi2D::Surface& surface, u32 bytes_per_pixel) -> size_t {
        if (surface.linear == Fermi2D::MemoryLayout::BlockLinear) {
            return CalculateSize(true, bytes_per_pixel, surface.width, surface.height,
                                 surface.depth, surface.block_height, surface.block_depth);
        }
        const u64 size = static_cast<u64>(surface.pitch) * surface.height;
        if (size > (std::numeric_limits<size_t>::max)()) {
            return 0;
        }
        return static_cast<size_t>(size);
    };

    const u32 src_extent_x = static_cast<u32>(config.src_x1 - config.src_x0);
    const u32 src_extent_y = static_cast<u32>(config.src_y1 - config.src_y0);

    const u32 dst_extent_x = static_cast<u32>(config.dst_x1 - config.dst_x0);
    const u32 dst_extent_y = static_cast<u32>(config.dst_y1 - config.dst_y0);
    // Both sizes below drive the row arithmetic of this software copy, and the destination ones
    // are used for writes. Guessing four bytes per pixel for an undecodable format makes a copy
    // write past the end of the bounded span, so refuse the blit instead of guessing. The
    // accelerated path does not use these and is not affected.
    const auto src_pixel_format = VideoCore::Surface::TryPixelFormatFromRenderTargetFormat(src.format);
    const auto dst_pixel_format = VideoCore::Surface::TryPixelFormatFromRenderTargetFormat(dst.format);
    if (!src_pixel_format || !dst_pixel_format) {
        static std::mutex mutex;
        static std::set<u32> logged;
        bool should_log = false;
        {
            const std::scoped_lock lock{mutex};
            const u32 undecoded = src_pixel_format ? static_cast<u32>(dst.format)
                                                  : static_cast<u32>(src.format);
            should_log = logged.size() < 8 && logged.emplace(undecoded).second;
        }
        if (should_log) {
            LOG_CRITICAL(HW_GPU,
                         "SoftwareBlit: undecodable {} format, skipping the copy instead of "
                         "guessing a pixel size",
                         src_pixel_format ? "destination" : "source");
        }
        return false;
    }
    const auto src_bytes_per_pixel = BytesPerBlock(*src_pixel_format);
    const auto dst_bytes_per_pixel = BytesPerBlock(*dst_pixel_format);
    const u64 src_row_pitch_u64 = static_cast<u64>(src_extent_x) * src_bytes_per_pixel;
    const u64 dst_row_pitch_u64 = static_cast<u64>(dst_extent_x) * dst_bytes_per_pixel;
    if (src_row_pitch_u64 > (std::numeric_limits<u32>::max)() ||
        dst_row_pitch_u64 > (std::numeric_limits<u32>::max)()) {
        return false;
    }
    if (src.linear != Fermi2D::MemoryLayout::BlockLinear &&
        static_cast<u64>(src.pitch) < static_cast<u64>(config.src_x1) * src_bytes_per_pixel) {
        return false;
    }
    if (dst.linear != Fermi2D::MemoryLayout::BlockLinear &&
        static_cast<u64>(dst.pitch) < static_cast<u64>(config.dst_x1) * dst_bytes_per_pixel) {
        return false;
    }
    const size_t src_size = get_surface_size(src, src_bytes_per_pixel);
    const size_t dst_size = get_surface_size(dst, dst_bytes_per_pixel);
    const u64 src_pixels_u64 = static_cast<u64>(src_extent_x) * src_extent_y;
    const u64 dst_pixels_u64 = static_cast<u64>(dst_extent_x) * dst_extent_y;
    const u64 src_copy_u64 = src_pixels_u64 * src_bytes_per_pixel;
    const u64 dst_copy_u64 = dst_pixels_u64 * dst_bytes_per_pixel;
    if (src_size == 0 || dst_size == 0 || src_copy_u64 == 0 || dst_copy_u64 == 0 ||
        src_copy_u64 > (std::numeric_limits<size_t>::max)() ||
        dst_copy_u64 > (std::numeric_limits<size_t>::max)() ||
        src_pixels_u64 > (std::numeric_limits<size_t>::max)() / ir_components ||
        dst_pixels_u64 > (std::numeric_limits<size_t>::max)() / ir_components) {
        return false;
    }

    const SubSurface src_part = CoveredRows(src, src_bytes_per_pixel, config.src_x0, config.src_y0,
                                            src_extent_x, src_extent_y, src_size);
    const SubSurface dst_part = CoveredRows(dst, dst_bytes_per_pixel, config.dst_x0, config.dst_y0,
                                            dst_extent_x, dst_extent_y, dst_size);
    const auto part_valid = [](const Fermi2D::Surface& surface, const SubSurface& part, s32 x1,
                               u32 extent_y, u32 bpp) {
        if (part.size == 0) {
            return false;
        }
        if (surface.linear == Fermi2D::MemoryLayout::BlockLinear) {
            return part.origin_y <= part.height && extent_y <= part.height - part.origin_y;
        }
        const u64 needed =
            (static_cast<u64>(part.origin_y) + extent_y - 1) * surface.pitch +
            static_cast<u64>(x1) * bpp;
        return needed <= part.size;
    };
    if (!part_valid(src, src_part, config.src_x1, src_extent_y, src_bytes_per_pixel) ||
        !part_valid(dst, dst_part, config.dst_x1, dst_extent_y, dst_bytes_per_pixel)) {
        return false;
    }

    Tegra::Memory::GpuGuestMemory<u8, Tegra::Memory::GuestMemoryFlags::SafeRead> tmp_buffer(
        memory_manager, src.Address() + src_part.offset, src_part.size, &impl->tmp_buffer);

    const size_t src_copy_size = static_cast<size_t>(src_copy_u64);
    const size_t dst_copy_size = static_cast<size_t>(dst_copy_u64);

    impl->src_buffer.resize_destructive(src_copy_size);

    const bool no_passthrough =
        src.format != dst.format || src_extent_x != dst_extent_x || src_extent_y != dst_extent_y;

    const auto conversion_phase_same_format = [&]() {
        NearestNeighbor(impl->src_buffer, impl->dst_buffer, src_extent_x, src_extent_y,
                        dst_extent_x, dst_extent_y, dst_bytes_per_pixel);
    };

    const auto conversion_phase_ir = [&]() {
        auto* input_converter = impl->converter_factory.GetFormatConverter(src.format);
        auto* output_converter = impl->converter_factory.GetFormatConverter(dst.format);
        impl->intermediate_src.resize_destructive(static_cast<size_t>(src_pixels_u64) *
                                                  ir_components);
        impl->intermediate_dst.resize_destructive(static_cast<size_t>(dst_pixels_u64) *
                                                  ir_components);
        input_converter->ConvertTo(impl->src_buffer, impl->intermediate_src);

        if (config.filter != Fermi2D::Filter::Bilinear) {
            NearestNeighborFast(impl->intermediate_src, impl->intermediate_dst, src_extent_x,
                                src_extent_y, dst_extent_x, dst_extent_y);
        } else {
            Bilinear(impl->intermediate_src, impl->intermediate_dst, src_extent_x, src_extent_y,
                     dst_extent_x, dst_extent_y);
        }

        output_converter->ConvertFrom(impl->intermediate_dst, impl->dst_buffer);
    };

    // Do actual Blit

    impl->dst_buffer.resize_destructive(dst_copy_size);
    if (src.linear == Fermi2D::MemoryLayout::BlockLinear) {
        UnswizzleSubrect(impl->src_buffer, tmp_buffer, src_bytes_per_pixel, src.width,
                         src_part.height, src.depth, config.src_x0, src_part.origin_y,
                         src_extent_x, src_extent_y, src.block_height, src.block_depth,
                         src_extent_x * src_bytes_per_pixel, memory_manager.UsesNativeTiling());
    } else {
        ProcessPitchLinear<false>(tmp_buffer, impl->src_buffer, src_extent_x, src_extent_y,
                                  src.pitch, config.src_x0, src_part.origin_y, src_bytes_per_pixel);
    }

    // Conversion Phase
    if (no_passthrough) {
        if (src.format != dst.format || config.filter == Fermi2D::Filter::Bilinear) {
            conversion_phase_ir();
        } else {
            conversion_phase_same_format();
        }
    } else {
        impl->dst_buffer.swap(impl->src_buffer);
    }

    {
        Tegra::Memory::GpuGuestMemoryScoped<u8, Tegra::Memory::GuestMemoryFlags::SafeReadWrite>
            tmp_buffer2(memory_manager, dst.Address() + dst_part.offset, dst_part.size,
                        &impl->tmp_buffer);

        if (dst.linear == Fermi2D::MemoryLayout::BlockLinear) {
            SwizzleSubrect(tmp_buffer2, impl->dst_buffer, dst_bytes_per_pixel, dst.width,
                           dst_part.height, dst.depth, config.dst_x0, dst_part.origin_y,
                           dst_extent_x, dst_extent_y, dst.block_height, dst.block_depth,
                           dst_extent_x * dst_bytes_per_pixel, memory_manager.UsesNativeTiling());
        } else {
            ProcessPitchLinear<true>(impl->dst_buffer, tmp_buffer2, dst_extent_x, dst_extent_y,
                                     dst.pitch, config.dst_x0, dst_part.origin_y,
                                     static_cast<size_t>(dst_bytes_per_pixel));
        }
    } // tmp_buffer2 writes the destination back here
    impl->ReleaseLargeBuffers();
    return true;
}

} // namespace Tegra::Engines::Blitter
