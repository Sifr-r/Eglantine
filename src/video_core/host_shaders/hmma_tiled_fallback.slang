// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Tiled FP16 matrix multiply-accumulate compute fallback (D = A * B + C)
// matching Ampere HMMA.16816.F16 tile geometry (M=16, N=8, K=16).

struct Params {
    uint m;
    uint n;
    uint k;
    uint lda_words;
    uint ldb_words;
    uint ldc_words;
    uint ldd_words;
    uint flags;
};

[[vk::push_constant]]
ConstantBuffer<Params> params;

[[vk::binding(0, 0)]]
StructuredBuffer<uint> mat_a;

[[vk::binding(1, 0)]]
StructuredBuffer<uint> mat_b;

[[vk::binding(2, 0)]]
StructuredBuffer<uint> mat_c;

[[vk::binding(3, 0)]]
RWStructuredBuffer<uint> mat_d;

groupshared float tile_a[16][16];
groupshared float tile_b[16][8];
groupshared float tile_out[16][8];

float UnpackHalf(uint packed_word, uint lane) {
    uint bits = (lane == 0u) ? (packed_word & 0xFFFFu) : (packed_word >> 16u);
    return f16tof32(bits);
}

[numthreads(8, 16, 1)]
void main(uint3 group_id : SV_GroupID, uint3 local_id : SV_GroupThreadID) {
    uint tile_row = group_id.y * 16u;
    uint tile_col = group_id.x * 8u;
    uint row = tile_row + local_id.y;
    uint col = tile_col + local_id.x;

    float acc = 0.0;
    if (row < params.m && col < params.n) {
        uint c_word = mat_c[row * params.ldc_words + (col >> 1u)];
        acc = UnpackHalf(c_word, col & 1u);
    }

    uint k_tiles = (params.k + 15u) >> 4u;
    for (uint kt = 0u; kt < k_tiles; ++kt) {
        uint k_base = kt * 16u;

        // Each thread in the 8x16 workgroup loads 2 FP16 elements along K into tile_a[local_id.y][local_id.x*2 + 0..1]
        uint a_k = k_base + local_id.x * 2u;
        float a0 = 0.0;
        float a1 = 0.0;
        if (row < params.m && a_k < params.k) {
            uint a_word = mat_a[row * params.lda_words + (a_k >> 1u)];
            a0 = f16tof32(a_word & 0xFFFFu);
            if (a_k + 1u < params.k) {
                a1 = f16tof32(a_word >> 16u);
            }
        }
        tile_a[local_id.y][local_id.x * 2u] = a0;
        tile_a[local_id.y][local_id.x * 2u + 1u] = a1;

        // Each thread loads one element of B[k_base + local_id.y, col]
        uint b_k = k_base + local_id.y;
        float b_val = 0.0;
        if (b_k < params.k && col < params.n) {
            uint b_word = mat_b[b_k * params.ldb_words + (col >> 1u)];
            b_val = UnpackHalf(b_word, col & 1u);
        }
        tile_b[local_id.y][local_id.x] = b_val;

        GroupMemoryBarrierWithGroupSync();

        for (uint kk = 0u; kk < 16u; ++kk) {
            acc += tile_a[local_id.y][kk] * tile_b[kk][local_id.x];
        }

        GroupMemoryBarrierWithGroupSync();
    }

    // Store packed half2 words safely via even-column threads sharing neighbor's accumulator
    tile_out[local_id.y][local_id.x] = acc;
    GroupMemoryBarrierWithGroupSync();

    if ((local_id.x & 1u) == 0u && row < params.m && col < params.n) {
        uint lo = f32tof16(tile_out[local_id.y][local_id.x]) & 0xFFFFu;
        uint hi = (col + 1u < params.n) ? (f32tof16(tile_out[local_id.y][local_id.x + 1u]) & 0xFFFFu) : 0u;
        mat_d[row * params.ldd_words + (col >> 1u)] = lo | (hi << 16u);
    }
}
