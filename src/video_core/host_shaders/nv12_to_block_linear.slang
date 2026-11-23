// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Converts linear NV12 luma (Y) or chroma (UV) planes into Tegra BlockLinear GOB memory,
// supporting Switch 2 native_tiling GOB swizzle bit XOR when enabled.

struct Nv12SwizzleParams {
    uint width_bytes;
    uint height;
    uint src_pitch_bytes;
    uint src_byte_offset;
    uint words_per_row;
    uint block_size;
    uint x_shift;
    uint block_height;
    uint block_height_mask;
    uint dst_word_offset;
    uint dst_word_limit;
    uint native_tiling;
};

[[vk::push_constant]]
ConstantBuffer<Nv12SwizzleParams> params;

[[vk::binding(0, 0)]]
StructuredBuffer<uint> src_linear;

[[vk::binding(1, 0)]]
RWStructuredBuffer<uint> dst_block_linear;

uint ReadSourceWord(uint byte_offset) {
    uint word_idx = byte_offset >> 2u;
    uint shift = (byte_offset & 3u) * 8u;
    uint lo = src_linear[word_idx];
    if (shift == 0u) {
        return lo;
    }
    uint hi = src_linear[word_idx + 1u];
    return (lo >> shift) | (hi << (32u - shift));
}

[numthreads(256, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
    uint word_index = dispatch_id.x;
    uint total_words = params.words_per_row * params.height;
    if (word_index >= total_words) {
        return;
    }

    uint x_word = word_index % params.words_per_row;
    uint y = word_index / params.words_per_row;
    uint x_byte = x_word * 4u;

    uint src_byte = params.src_byte_offset + y * params.src_pitch_bytes + x_byte;
    uint value = (x_byte < params.width_bytes) ? ReadSourceWord(src_byte) : 0u;

    uint gob = ((x_byte & 32u) << 3u) | ((y & 6u) << 5u) | ((x_byte & 16u) << 1u) |
               ((y & 1u) << 4u) | (x_byte & 15u);
    if (params.native_tiling != 0u) {
        uint difference = ((gob >> 5u) ^ (gob >> 6u)) & 1u;
        gob ^= (difference << 5u) | (difference << 6u);
    }

    uint block_y = y >> 3u;
    uint address = (block_y >> params.block_height) * params.block_size +
                   ((block_y & params.block_height_mask) << 9u) +
                   ((x_byte >> 6u) << params.x_shift) + gob;
    uint dst_word = params.dst_word_offset + (address >> 2u);
    if (dst_word < params.dst_word_limit) {
        dst_block_linear[dst_word] = value;
    }
}
