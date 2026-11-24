// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <limits>
#include <stb_dxt.h>
#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/textures/bcn.h"
#include "video_core/textures/workers.h"

namespace Tegra::Texture::BCN {

using BCNCompressor = void(u8* block_output, const u8* block_input, bool any_alpha);

template <u32 BytesPerBlock, bool ThresholdAlpha = false>
void CompressBCN(std::span<const uint8_t> data, uint32_t width, uint32_t height, uint32_t depth,
                 std::span<uint8_t> output, BCNCompressor f) {
    constexpr u8 alpha_threshold = 128;
    constexpr size_t bytes_per_px = 4;
    if (width == 0 || height == 0 || depth == 0) {
        return;
    }

    if (static_cast<size_t>(width) > (std::numeric_limits<size_t>::max)() / height) {
        LOG_ERROR(HW_GPU, "BCN: input dimensions overflow for {}x{}", width, height);
        return;
    }
    const size_t plane_dim = static_cast<size_t>(width) * height;
    if (plane_dim > (std::numeric_limits<size_t>::max)() / depth) {
        LOG_ERROR(HW_GPU, "BCN: input pixel count overflow for {}x{}x{}", width, height, depth);
        return;
    }
    const size_t total_pixels = plane_dim * depth;
    if (total_pixels > (std::numeric_limits<size_t>::max)() / bytes_per_px) {
        LOG_ERROR(HW_GPU, "BCN: input byte count overflow");
        return;
    }
    const size_t required_input = total_pixels * bytes_per_px;
    if (data.size() < required_input) {
        LOG_ERROR(HW_GPU, "BCN: truncated input, got {} bytes but need {}", data.size(),
                  required_input);
        return;
    }

    const size_t cols = ((static_cast<size_t>(width) - 1) / 4) + 1;
    const size_t rows = ((static_cast<size_t>(height) - 1) / 4) + 1;
    if (cols > (std::numeric_limits<size_t>::max)() / BytesPerBlock) {
        LOG_ERROR(HW_GPU, "BCN: row stride overflow");
        return;
    }
    const size_t bytes_per_row = static_cast<size_t>(BytesPerBlock) * cols;
    if (rows != 0 && bytes_per_row > (std::numeric_limits<size_t>::max)() / rows) {
        LOG_ERROR(HW_GPU, "BCN: plane size overflow");
        return;
    }
    const size_t bytes_per_plane = bytes_per_row * rows;
    if (bytes_per_plane > (std::numeric_limits<size_t>::max)() / depth) {
        LOG_ERROR(HW_GPU, "BCN: output byte count overflow");
        return;
    }
    const size_t required_output = bytes_per_plane * depth;
    if (output.size() < required_output) {
        LOG_ERROR(HW_GPU, "BCN: output buffer too small, got {} bytes but need {}", output.size(),
                  required_output);
        return;
    }

    Common::ThreadWorker& workers{GetThreadWorkers()};

    for (u32 z = 0; z < depth; z++) {
        for (u32 y = 0; y < height; y += 4) {
            auto compress_row = [z, y, width, height, plane_dim, bytes_per_row, bytes_per_plane, f,
                                 data, output]() {
                for (u32 x = 0; x < width; x += 4) {
                    // Gather 4x4 block of RGBA texels
                    u8 input_colors[4][4][4];
                    bool any_alpha = false;

                    for (u32 j = 0; j < 4; j++) {
                        for (u32 i = 0; i < 4; i++) {
                            if ((x + i < width) && (y + j < height)) {
                                const size_t coord =
                                    (static_cast<size_t>(z) * plane_dim +
                                     (static_cast<size_t>(y) + j) * width +
                                     (static_cast<size_t>(x) + i)) *
                                    bytes_per_px;
                                if constexpr (ThresholdAlpha) {
                                    if (data[coord + 3] >= alpha_threshold) {
                                        input_colors[j][i][0] = data[coord + 0];
                                        input_colors[j][i][1] = data[coord + 1];
                                        input_colors[j][i][2] = data[coord + 2];
                                        input_colors[j][i][3] = 255;
                                    } else {
                                        any_alpha = true;
                                        std::memset(input_colors[j][i], 0, bytes_per_px);
                                    }
                                } else {
                                    std::memcpy(input_colors[j][i], &data[coord], bytes_per_px);
                                }
                            } else {
                                std::memset(input_colors[j][i], 0, bytes_per_px);
                            }
                        }
                    }

                    const size_t out_offset = static_cast<size_t>(z) * bytes_per_plane +
                                              static_cast<size_t>(y / 4) * bytes_per_row +
                                              static_cast<size_t>(x / 4) * BytesPerBlock;
                    f(output.data() + out_offset, reinterpret_cast<u8*>(input_colors), any_alpha);
                }
            };
            workers.QueueWork(std::move(compress_row));
        }
        workers.WaitForRequests();
    }
}

void CompressBC1(std::span<const uint8_t> data, uint32_t width, uint32_t height, uint32_t depth,
                 std::span<uint8_t> output) {
    CompressBCN<8, true>(data, width, height, depth, output,
                         [](u8* block_output, const u8* block_input, bool any_alpha) {
                             stb_compress_bc1_block(block_output, block_input, any_alpha,
                                                    STB_DXT_NORMAL);
                         });
}

void CompressBC3(std::span<const uint8_t> data, uint32_t width, uint32_t height, uint32_t depth,
                 std::span<uint8_t> output) {
    CompressBCN<16, false>(data, width, height, depth, output,
                           [](u8* block_output, const u8* block_input, bool any_alpha) {
                               stb_compress_bc3_block(block_output, block_input, STB_DXT_NORMAL);
                           });
}

} // namespace Tegra::Texture::BCN
