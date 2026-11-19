// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The expected values are the worked examples of the Khronos Data Format Specification,
// section 22 "ETC2 Compressed Texture Image Formats". Every block is built bit by bit from the
// field layout of tables 22.1, 22.5 and 22.7.

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/surface.h"
#include "video_core/texture_cache/util.h"
#include "video_core/textures/etc2.h"

namespace {

using Tegra::Texture::ETC2::BytesPerTexel;
using Tegra::Texture::ETC2::Format;

constexpr u32 BlockPixels = 16;

/// Writes `value` into the bits `hi` down to `lo` of a block.
void SetBits(u64& block, u32 hi, u32 lo, u32 value) {
    const u64 mask = ((u64{1} << (hi - lo + 1)) - 1) << lo;
    block = (block & ~mask) | ((static_cast<u64>(value) << lo) & mask);
}

/// Writes the two bit pixel index of the texel `index` of a block. The specification stores the
/// least significant bit in the lower half of the block and the most significant bit in the
/// upper half.
void SetPixelIndex(u64& block, u32 index, u32 value) {
    SetBits(block, index, index, value & 1);
    SetBits(block, 16 + index, 16 + index, (value >> 1) & 1);
}

/// Writes the three bit alpha / EAC index of column-major texel `pixel` (`x * 4 + y`).
void SetAlphaIndex(u64& block, u32 pixel, u32 value) {
    SetBits(block, 47 - 3 * pixel, 45 - 3 * pixel, value);
}

/// Builds a 64-bit alpha / EAC block with all 16 texels initialized to `default_index`.
u64 MakeAlphaOrEacBlock(u32 base, u32 multiplier, u32 table, u32 default_index) {
    u64 block = 0;
    SetBits(block, 63, 56, base);
    SetBits(block, 55, 52, multiplier);
    SetBits(block, 51, 48, table);
    for (u32 p = 0; p < BlockPixels; ++p) {
        SetAlphaIndex(block, p, default_index);
    }
    return block;
}

/// Builds a 16-byte RGBA block from an alpha word and a colour word.
std::array<u8, 16> MakePairBlock(u64 first, u64 second) {
    std::array<u8, 16> block{};
    for (u32 i = 0; i < 8; ++i) {
        block[i] = static_cast<u8>(first >> (56 - 8 * i));
        block[8 + i] = static_cast<u8>(second >> (56 - 8 * i));
    }
    return block;
}

/// Floor division by 4 for ETC2 planar mode verification.
s32 PlanarDiv4(s32 value) {
    return value >= 0 ? value / 4 : -((-value + 3) / 4);
}

/// Turns a block into the eight bytes it is stored as, the first byte being the least
/// significant one.
std::array<u8, 8> ToBytes(u64 block) {
    std::array<u8, 8> bytes{};
    for (u32 i = 0; i < 8; ++i) {
        bytes[i] = static_cast<u8>(block >> (56 - 8 * i));
    }
    return bytes;
}

/// Decodes a single 4x4 block.
std::vector<u8> DecodeBlock(Format format, const std::array<u8, 8>& block) {
    const u32 stride = BytesPerTexel(format);
    std::vector<u8> output(BlockPixels * stride, 0);
    Tegra::Texture::ETC2::Decompress(block, format, 4, 4, 1, output);
    return output;
}

/// The colour of the texel at (x, y) of a decoded block.
std::array<u8, 4> Texel(const std::vector<u8>& output, u32 x, u32 y) {
    const u32 stride = BytesPerTexel(Format::RGB);
    const size_t offset = (static_cast<size_t>(y) * 4 + x) * stride;
    return {output[offset + 0], output[offset + 1], output[offset + 2], output[offset + 3]};
}

/// The colour a base channel and a modifier produce. The specification clamps the sum to the
/// eight bit range, so a base colour near the top of the range saturates instead of wrapping.
u8 Modified(s32 base, s32 modifier) {
    return static_cast<u8>(std::clamp(base + modifier, 0, 255));
}

/// The 'T' mode block of the worked example of figure 22.7. The red channel of the first base
/// colour is (13, 1, 8) and of the second one (4, 12, 13), the distance index is 5 and
/// selects a distance of 32.
///
/// The bits 63 to 59 are read as the red base colour of a differential block and the bits 58
/// to 56 as its red delta. Here they hold 31 and 1, whose sum leaves the five bit range, so
/// the block is not a differential block but the 'T' mode block the same bits describe.
u64 TModeBlock() {
    u64 block = 0;
    SetBits(block, 63, 61, 7);   // Don't care, makes the red sum overflow.
    SetBits(block, 60, 59, 3);   // R^3..2 of 13.
    SetBits(block, 58, 56, 1);   // Don't care, the red sum of 31 and 1 overflows.
    SetBits(block, 55, 52, 1);   // G.
    SetBits(block, 51, 48, 8);   // B.
    SetBits(block, 47, 44, 4);   // R2.
    SetBits(block, 43, 40, 12);  // G2.
    SetBits(block, 39, 36, 13);  // B2.
    SetBits(block, 35, 34, 2);   // da, the distance index is (da << 1) | db.
    SetBits(block, 33, 33, 1);
    SetBits(block, 32, 32, 1);   // db.
    for (u32 i = 0; i < BlockPixels; ++i) {
        SetPixelIndex(block, i, i % 4);
    }
    return block;
}

} // Anonymous namespace

TEST_CASE("ETC2 T mode block decodes to its paint colours", "[video_core][textures][etc2]") {
    const auto output = DecodeBlock(Format::RGB, ToBytes(TModeBlock()));

    // The paint colours are the first base colour (13, 1, 8) extended to (221, 17, 136), the
    // second base colour (4, 12, 13) extended to (68, 204, 221), that colour plus the distance
    // of 32 and that colour minus the distance of 32.
    const std::array<std::array<u8, 3>, 4> paint{{
        {{221, 17, 136}},
        {{100, 236, 253}},
        {{68, 204, 221}},
        {{36, 172, 189}},
    }};

    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            // The texels are numbered column major, the index selects the paint colour.
            const auto& expected = paint[(x * 4 + y) % 4];
            const auto texel = Texel(output, x, y);
            REQUIRE(texel[0] == expected[0]);
            REQUIRE(texel[1] == expected[1]);
            REQUIRE(texel[2] == expected[2]);
            REQUIRE(texel[3] == 255);
        }
    }
}

TEST_CASE("ETC2 differential overflow decodes as the T mode block", "[video_core][textures][etc2]") {
    // The same block read as a differential block would have a red base colour of 31 and a red
    // delta of 1, which is not a valid five bit sum. Such a block is decoded as the 'T' mode
    // block its bits describe.
    const u64 block = TModeBlock();
    const auto output = DecodeBlock(Format::RGB, ToBytes(block));

    const std::array<std::array<u8, 3>, 4> paint{{
        {{221, 17, 136}},
        {{100, 236, 253}},
        {{68, 204, 221}},
        {{36, 172, 189}},
    }};

    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const auto& expected = paint[(x * 4 + y) % 4];
            const auto texel = Texel(output, x, y);
            REQUIRE(texel[0] == expected[0]);
            REQUIRE(texel[1] == expected[1]);
            REQUIRE(texel[2] == expected[2]);
        }
    }

    // A differential block whose sums all fit is decoded as such and not as a 'T' mode block.
    u64 valid = 0;
    SetBits(valid, 63, 59, 29);
    SetBits(valid, 58, 56, 4); // -4
    SetBits(valid, 55, 51, 26);
    SetBits(valid, 50, 48, 5); // -3
    SetBits(valid, 47, 43, 8);
    SetBits(valid, 42, 40, 3); // +3
    SetBits(valid, 39, 37, 2);
    SetBits(valid, 36, 34, 3);
    SetBits(valid, 33, 33, 1);
    for (u32 i = 0; i < BlockPixels; ++i) {
        SetPixelIndex(valid, i, 1);
    }
    const auto differential = DecodeBlock(Format::RGB, ToBytes(valid));

    // The base colours are (29, 26, 8) and (25, 23, 11) extended to (239, 214, 66) and
    // (206, 189, 90), the modifier tables 2 and 3 add 29 and 42 to them.
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const bool first = x < 2;
            const auto texel = Texel(differential, x, y);
            REQUIRE(texel[0] == (first ? 255 : 248));
            REQUIRE(texel[1] == (first ? 243 : 231));
            REQUIRE(texel[2] == (first ? 95 : 132));
        }
    }
}

TEST_CASE("ETC2 single colour block decodes to one colour", "[video_core][textures][etc2]") {
    // A differential block whose deltas are all zero has the same base colour in both
    // subblocks.
    u64 block = 0;
    SetBits(block, 63, 59, 20);
    SetBits(block, 55, 51, 10);
    SetBits(block, 47, 43, 5);
    SetBits(block, 39, 37, 0);
    SetBits(block, 36, 34, 0);
    SetBits(block, 33, 33, 1);
    for (u32 i = 0; i < BlockPixels; ++i) {
        SetPixelIndex(block, i, 1);
    }
    const auto output = DecodeBlock(Format::RGB, ToBytes(block));

    // The base colour (20, 10, 5) extends to (165, 82, 41) and the modifier table 0 adds 8.
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const auto texel = Texel(output, x, y);
            REQUIRE(texel[0] == 173);
            REQUIRE(texel[1] == 90);
            REQUIRE(texel[2] == 49);
            REQUIRE(texel[3] == 255);
        }
    }
}

TEST_CASE("ETC2 punchthrough alpha reserves one pixel index", "[video_core][textures][etc2]") {
    // A punchthrough block whose opaque bit is unset reserves the pixel index 2 for
    // transparency and zeroes the colour of the texels that use it.
    u64 block = 0;
    SetBits(block, 63, 59, 29);
    SetBits(block, 58, 56, 4); // -4
    SetBits(block, 55, 51, 26);
    SetBits(block, 50, 48, 5); // -3
    SetBits(block, 47, 43, 8);
    SetBits(block, 42, 40, 3); // +3
    SetBits(block, 39, 37, 2);
    SetBits(block, 36, 34, 3);
    SetBits(block, 33, 33, 0); // The block is not fully opaque.
    for (u32 i = 0; i < BlockPixels; ++i) {
        SetPixelIndex(block, i, i % 4);
    }
    const auto output = DecodeBlock(Format::RGB_PTA, ToBytes(block));

    // The first subblock has the base colour (239, 214, 66) and the modifier table 2, the
    // second one the base colour (206, 189, 90) and the modifier table 3. A punchthrough
    // block that is not opaque only shifts the colour by the large modifier. The shifted colour
    // is clamped to the eight bit range, so the first subblock saturates on its red channel.
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u32 index = (x * 4 + y) % 4;
            const bool first = x < 2;
            const auto texel = Texel(output, x, y);
            if (index == 2) {
                REQUIRE(texel[0] == 0);
                REQUIRE(texel[1] == 0);
                REQUIRE(texel[2] == 0);
                REQUIRE(texel[3] == 0);
            } else {
                const s32 modifier = index == 1 ? (first ? 29 : 42) : (index == 3 ? (first ? -29 : -42) : 0);
                REQUIRE(texel[0] == Modified(first ? 239 : 206, modifier));
                REQUIRE(texel[1] == Modified(first ? 214 : 189, modifier));
                REQUIRE(texel[2] == Modified(first ? 66 : 90, modifier));
                REQUIRE(texel[3] == 255);
            }
        }
    }
}

TEST_CASE("ETC2 RGBA block decodes alpha in column-major block order to row-major output",
          "[video_core][textures][etc2]") {
    // The alpha half of the block comes first, the colour half second.
    // Texel indices are column-major p = x * 4 + y:
    //   p=0 -> (0,0), p=1 -> (0,1), p=4 -> (1,0), p=11 -> (2,3).
    // With base=103, multiplier=2, table=13 ({-1, -2, -3, -10, 0, 1, 2, 9}):
    //   index 3 -> 83, index 4 -> 103, index 7 -> 121, index 5 -> 105.
    u64 alpha = MakeAlphaOrEacBlock(103, 2, 13, 4);
    SetAlphaIndex(alpha, 0, 3);  // (0, 0) -> 83
    SetAlphaIndex(alpha, 1, 4);  // (0, 1) -> 103
    SetAlphaIndex(alpha, 4, 7);  // (1, 0) -> 121
    SetAlphaIndex(alpha, 11, 5); // (2, 3) -> 105

    u64 color = 0;
    SetBits(color, 63, 60, 4);
    SetBits(color, 59, 56, 14);
    SetBits(color, 55, 52, 11);
    SetBits(color, 51, 48, 3);
    SetBits(color, 47, 44, 9);
    SetBits(color, 43, 40, 8);
    SetBits(color, 39, 37, 4);
    for (u32 i = 0; i < BlockPixels; ++i) {
        SetPixelIndex(color, i, 1);
    }

    const std::array<u8, 16> block = MakePairBlock(alpha, color);
    std::vector<u8> output(BlockPixels * 4, 0);
    Tegra::Texture::ETC2::Decompress(block, Format::RGBA, 4, 4, 1, output);

    // Output is row-major (y * 4 + x) * 4 + 3.
    REQUIRE(output[(0 * 4 + 0) * 4 + 3] == 83);  // output[3]
    REQUIRE(output[(1 * 4 + 0) * 4 + 3] == 103); // (0, 1) -> output[19]
    REQUIRE(output[(0 * 4 + 1) * 4 + 3] == 121); // (1, 0) -> output[7]
    REQUIRE(output[(3 * 4 + 2) * 4 + 3] == 105); // (2, 3) -> output[59]
    // The colour half is the 'individual' mode block of the other test.
    REQUIRE(output[0] == 128);
    REQUIRE(output[1] == 247);
    REQUIRE(output[2] == 213);
}

TEST_CASE("ETC2 EAC blocks decode their channels with non-diagonal texels",
          "[video_core][textures][etc2]") {
    // Table 13: {-1, -2, -3, -10, 0, 1, 2, 9}.
    // Unsigned EAC with base=103, multiplier=2: out = clamp(103*8 + 4 + mod*16, 0, 2047) >> 3.
    u64 red = MakeAlphaOrEacBlock(103, 2, 13, 5); // default: 105
    SetAlphaIndex(red, 0, 3);                     // (0, 0) -> 83
    SetAlphaIndex(red, 1, 4);                     // (0, 1) -> 103
    SetAlphaIndex(red, 4, 7);                     // (1, 0) -> 121
    SetAlphaIndex(red, 11, 6);                    // (2, 3) -> 107

    u64 green = MakeAlphaOrEacBlock(200, 2, 13, 5); // default: 202
    SetAlphaIndex(green, 0, 3);                     // (0, 0) -> 180
    SetAlphaIndex(green, 1, 7);                     // (0, 1) -> 218
    SetAlphaIndex(green, 4, 2);                     // (1, 0) -> 194
    SetAlphaIndex(green, 11, 4);                    // (2, 3) -> 200

    const std::array<u8, 16> block = MakePairBlock(red, green);
    std::vector<u8> output(BlockPixels * 2, 0);
    Tegra::Texture::ETC2::Decompress(block, Format::R11G11, 4, 4, 1, output);

    REQUIRE(output[(0 * 4 + 0) * 2 + 0] == 83);
    REQUIRE(output[(0 * 4 + 0) * 2 + 1] == 180);
    REQUIRE(output[(1 * 4 + 0) * 2 + 0] == 103); // (0, 1)
    REQUIRE(output[(1 * 4 + 0) * 2 + 1] == 218); // (0, 1)
    REQUIRE(output[(0 * 4 + 1) * 2 + 0] == 121); // (1, 0)
    REQUIRE(output[(0 * 4 + 1) * 2 + 1] == 194); // (1, 0)
    REQUIRE(output[(3 * 4 + 2) * 2 + 0] == 107); // (2, 3)
    REQUIRE(output[(3 * 4 + 2) * 2 + 1] == 200); // (2, 3)

    // The single channel format only carries the first half of the block.
    const auto single = DecodeBlock(Format::R11, ToBytes(red));
    REQUIRE(single[0 * 4 + 0] == 83);
    REQUIRE(single[1 * 4 + 0] == 103); // (0, 1)
    REQUIRE(single[0 * 4 + 1] == 121); // (1, 0)
    REQUIRE(single[3 * 4 + 2] == 107); // (2, 3)
    REQUIRE(single[2 * 4 + 3] == 105); // (3, 2)
}

TEST_CASE("ETC2 signed EAC blocks decode non-diagonal channels", "[video_core][textures][etc2]") {
    // base = -10 (0xF6), multiplier = 2, table = 13:
    //   index 3 (-10) -> -30, index 4 (0) -> -10, index 7 (+9) -> +8, index 2 (-3) -> -16.
    u64 red = MakeAlphaOrEacBlock(0xF6, 2, 13, 5); // default: -8
    SetAlphaIndex(red, 0, 3);                      // (0, 0) -> -30
    SetAlphaIndex(red, 1, 4);                      // (0, 1) -> -10
    SetAlphaIndex(red, 4, 7);                      // (1, 0) -> +8
    SetAlphaIndex(red, 11, 2);                     // (2, 3) -> -16

    const auto single = DecodeBlock(Format::R11_SNORM, ToBytes(red));
    REQUIRE(static_cast<s8>(single[0 * 4 + 0]) == -30);
    REQUIRE(static_cast<s8>(single[1 * 4 + 0]) == -10); // (0, 1)
    REQUIRE(static_cast<s8>(single[0 * 4 + 1]) == 8);   // (1, 0)
    REQUIRE(static_cast<s8>(single[3 * 4 + 2]) == -16); // (2, 3)
    REQUIRE(static_cast<s8>(single[2 * 4 + 3]) == -8);  // (3, 2)

    u64 green = MakeAlphaOrEacBlock(50, 2, 13, 5); // default: +52
    SetAlphaIndex(green, 0, 3);                    // (0, 0) -> +30
    SetAlphaIndex(green, 1, 7);                    // (0, 1) -> +68
    SetAlphaIndex(green, 4, 2);                    // (1, 0) -> +44
    SetAlphaIndex(green, 11, 4);                   // (2, 3) -> +50

    const std::array<u8, 16> block = MakePairBlock(red, green);
    std::vector<u8> output(BlockPixels * 2, 0);
    Tegra::Texture::ETC2::Decompress(block, Format::R11G11_SNORM, 4, 4, 1, output);

    REQUIRE(static_cast<s8>(output[(0 * 4 + 0) * 2 + 0]) == -30);
    REQUIRE(static_cast<s8>(output[(0 * 4 + 0) * 2 + 1]) == 30);
    REQUIRE(static_cast<s8>(output[(1 * 4 + 0) * 2 + 0]) == -10); // (0, 1)
    REQUIRE(static_cast<s8>(output[(1 * 4 + 0) * 2 + 1]) == 68);  // (0, 1)
    REQUIRE(static_cast<s8>(output[(0 * 4 + 1) * 2 + 0]) == 8);   // (1, 0)
    REQUIRE(static_cast<s8>(output[(0 * 4 + 1) * 2 + 1]) == 44);  // (1, 0)
    REQUIRE(static_cast<s8>(output[(3 * 4 + 2) * 2 + 0]) == -16); // (2, 3)
    REQUIRE(static_cast<s8>(output[(3 * 4 + 2) * 2 + 1]) == 50);  // (2, 3)
}

TEST_CASE("ETC2 H mode block decodes to its paint colours", "[video_core][textures][etc2]") {
    const auto build_h_block = [](u32 r1, u32 r2, u32 g2, u32 b2, u32 da, u32 db) {
        u64 block = 0;
        // R1 in 62..59, G1=15 (58..56=7 so r+rd stays in [0,31], bit 52=1), B1=7 (51=0, 49..47=7).
        // 55..53=7 and 50=0,49..48=3 yield g=30, gd=+3 -> g2=33 (overflows into H mode).
        SetBits(block, 63, 63, 0);
        SetBits(block, 62, 59, r1);
        SetBits(block, 58, 56, 7);
        SetBits(block, 55, 53, 7);
        SetBits(block, 52, 52, 1);
        SetBits(block, 51, 51, 0);
        SetBits(block, 50, 50, 0);
        SetBits(block, 49, 47, 7);
        SetBits(block, 46, 43, r2);
        SetBits(block, 42, 39, g2);
        SetBits(block, 38, 35, b2);
        SetBits(block, 34, 34, da);
        SetBits(block, 33, 33, 1);
        SetBits(block, 32, 32, db);
        for (u32 i = 0; i < BlockPixels; ++i) {
            SetPixelIndex(block, i, i % 4);
        }
        return block;
    };

    // Case 1: base1=(10, 15, 7) >= base2=(4, 8, 2) -> distance LSB=1, index=5 -> distance=32.
    {
        const auto output = DecodeBlock(Format::RGB, ToBytes(build_h_block(10, 4, 8, 2, 1, 0)));
        const std::array<std::array<u8, 3>, 4> paint{{
            {{202, 255, 151}},
            {{138, 223, 87}},
            {{100, 168, 66}},
            {{36, 104, 2}},
        }};
        for (u32 y = 0; y < 4; ++y) {
            for (u32 x = 0; x < 4; ++x) {
                const auto& expected = paint[(x * 4 + y) % 4];
                const auto texel = Texel(output, x, y);
                REQUIRE(texel[0] == expected[0]);
                REQUIRE(texel[1] == expected[1]);
                REQUIRE(texel[2] == expected[2]);
                REQUIRE(texel[3] == 255);
            }
        }
    }

    // Case 2: base1=(2, 15, 7) < base2=(12, 1, 5) -> distance LSB=0, index=2 -> distance=11.
    {
        const auto output = DecodeBlock(Format::RGB, ToBytes(build_h_block(2, 12, 1, 5, 0, 1)));
        const std::array<std::array<u8, 3>, 4> paint{{
            {{45, 255, 130}},
            {{23, 244, 108}},
            {{215, 28, 96}},
            {{193, 6, 74}},
        }};
        for (u32 y = 0; y < 4; ++y) {
            for (u32 x = 0; x < 4; ++x) {
                const auto& expected = paint[(x * 4 + y) % 4];
                const auto texel = Texel(output, x, y);
                REQUIRE(texel[0] == expected[0]);
                REQUIRE(texel[1] == expected[1]);
                REQUIRE(texel[2] == expected[2]);
                REQUIRE(texel[3] == 255);
            }
        }
    }
}

TEST_CASE("ETC2 Planar mode block interpolates asymmetrically", "[video_core][textures][etc2]") {
    u64 block = 0;
    // r=5, rd=0 -> r2=5; g=10, gd=+1 -> g2=11; b=31, bd=+3 -> b2=34 (overflows into Planar mode).
    SetBits(block, 63, 63, 0);
    SetBits(block, 62, 57, 20); // RO = 20 -> 81
    SetBits(block, 56, 56, 0);  // GO[6] = 0
    SetBits(block, 55, 55, 0);
    SetBits(block, 54, 49, 40); // GO[5:0] = 40 -> GO = 40 -> 80
    SetBits(block, 48, 48, 1);  // BO[5] = 1
    SetBits(block, 47, 45, 7);
    SetBits(block, 44, 43, 3);  // BO[4:3] = 3
    SetBits(block, 42, 42, 0);
    SetBits(block, 41, 39, 6);  // BO[2:0] = 6 -> BO = 62 -> 251
    SetBits(block, 38, 34, 25); // RH[5:1] = 25
    SetBits(block, 33, 33, 1);
    SetBits(block, 32, 32, 0);  // RH[0] = 0 -> RH = 50 -> 203
    SetBits(block, 31, 25, 90); // GH = 90 -> 181
    SetBits(block, 24, 19, 10); // BH = 10 -> 40
    SetBits(block, 18, 13, 10); // RV = 10 -> 40
    SetBits(block, 12, 6, 20);  // GV = 20 -> 40
    SetBits(block, 5, 0, 40);   // BV = 40 -> 162

    const auto output = DecodeBlock(Format::RGB, ToBytes(block));
    // Verify non-diagonal asymmetry: (1, 0) != (0, 1).
    REQUIRE(Texel(output, 1, 0)[0] != Texel(output, 0, 1)[0]);

    constexpr s32 r0 = 81, g0 = 80, b0 = 251;
    constexpr s32 rh = 203, gh = 181, bh = 40;
    constexpr s32 rv = 40, gv = 40, bv = 162;
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const s32 cx = static_cast<s32>(x);
            const s32 cy = static_cast<s32>(y);
            const u8 er = static_cast<u8>(
                std::clamp(PlanarDiv4(cx * (rh - r0) + cy * (rv - r0) + 4 * r0 + 2), 0, 255));
            const u8 eg = static_cast<u8>(
                std::clamp(PlanarDiv4(cx * (gh - g0) + cy * (gv - g0) + 4 * g0 + 2), 0, 255));
            const u8 eb = static_cast<u8>(
                std::clamp(PlanarDiv4(cx * (bh - b0) + cy * (bv - b0) + 4 * b0 + 2), 0, 255));
            const auto texel = Texel(output, x, y);
            REQUIRE(texel[0] == er);
            REQUIRE(texel[1] == eg);
            REQUIRE(texel[2] == eb);
            REQUIRE(texel[3] == 255);
        }
    }
}

TEST_CASE("ETC2 3D mipmapped ConvertImage matches ConvertedLayout across slices",
          "[video_core][textures][etc2]") {
    VideoCommon::ImageInfo info{};
    info.format = VideoCore::Surface::PixelFormat::ETC2_RGBA_UNORM;
    info.type = VideoCommon::ImageType::e3D;
    info.size = {8, 8, 4};
    info.resources = {.levels = 3, .layers = 1};

    // Level 0: 8x8x4 (16 blocks = 256B in, 1024B out)
    // Level 1: 4x4x2 ( 2 blocks =  32B in,  128B out)
    // Level 2: 2x2x1 ( 1 block  =  16B in,   16B out)
    REQUIRE(VideoCommon::CalculateConvertedSizeBytes(info) == 1024 + 128 + 16);

    const std::array<VideoCommon::BufferImageCopy, 3> base_copies{{
        {
            .buffer_offset = 0,
            .buffer_size = 256,
            .buffer_row_length = 8,
            .buffer_image_height = 8,
            .image_subresource = {.base_level = 0, .base_layer = 0, .num_layers = 1},
            .image_offset = {0, 0, 0},
            .image_extent = {8, 8, 4},
        },
        {
            .buffer_offset = 256,
            .buffer_size = 32,
            .buffer_row_length = 4,
            .buffer_image_height = 4,
            .image_subresource = {.base_level = 1, .base_layer = 0, .num_layers = 1},
            .image_offset = {0, 0, 0},
            .image_extent = {4, 4, 2},
        },
        {
            .buffer_offset = 288,
            .buffer_size = 16,
            .buffer_row_length = 4,
            .buffer_image_height = 4,
            .image_subresource = {.base_level = 2, .base_layer = 0, .num_layers = 1},
            .image_offset = {0, 0, 0},
            .image_extent = {2, 2, 1},
        },
    }};

    auto layout_copies = base_copies;
    VideoCommon::ConvertedLayout(info, layout_copies);

    std::vector<u8> input(304, 0);
    const std::array<u8, 3> level_bases{20, 100, 200};
    for (size_t lvl = 0; lvl < base_copies.size(); ++lvl) {
        u64 alpha = MakeAlphaOrEacBlock(level_bases[lvl], 1, 13, 4); // default: base + 0
        SetAlphaIndex(alpha, 1, 5); // (0, 1) -> base + 1
        SetAlphaIndex(alpha, 4, 7); // (1, 0) -> base + 9
        const auto block = MakePairBlock(alpha, 0);
        const size_t num_blocks = base_copies[lvl].buffer_size / 16;
        for (size_t b = 0; b < num_blocks; ++b) {
            std::copy(block.begin(), block.end(),
                      input.begin() + base_copies[lvl].buffer_offset + b * 16);
        }
    }

    auto convert_copies = base_copies;
    std::vector<u8> output(VideoCommon::CalculateConvertedSizeBytes(info), 0xcd);
    VideoCommon::ConvertImage(input, info, output, convert_copies);

    for (size_t lvl = 0; lvl < base_copies.size(); ++lvl) {
        REQUIRE(convert_copies[lvl].buffer_offset == layout_copies[lvl].buffer_offset);
        REQUIRE(convert_copies[lvl].buffer_size == layout_copies[lvl].buffer_size);
        REQUIRE(convert_copies[lvl].buffer_row_length == layout_copies[lvl].buffer_row_length);
        REQUIRE(convert_copies[lvl].buffer_image_height == layout_copies[lvl].buffer_image_height);

        const u32 w = convert_copies[lvl].image_extent.width;
        const u32 h = convert_copies[lvl].image_extent.height;
        const u32 d = convert_copies[lvl].image_extent.depth;
        for (u32 z = 0; z < d; ++z) {
            const size_t slice =
                convert_copies[lvl].buffer_offset + static_cast<size_t>(z) * w * h * 4;
            REQUIRE(output[slice + (1 * w + 0) * 4 + 3] == level_bases[lvl] + 1); // (0, 1)
            REQUIRE(output[slice + (0 * w + 1) * 4 + 3] == level_bases[lvl] + 9); // (1, 0)
        }
    }
}

TEST_CASE("ETC2 decodes partial blocks to the exact extent", "[video_core][textures][etc2]") {
    u64 block = 0;
    SetBits(block, 63, 60, 4);
    SetBits(block, 59, 56, 14);
    SetBits(block, 55, 52, 11);
    SetBits(block, 51, 48, 3);
    SetBits(block, 47, 44, 9);
    SetBits(block, 43, 40, 8);
    SetBits(block, 39, 37, 4);
    for (u32 i = 0; i < BlockPixels; ++i) {
        SetPixelIndex(block, i, 1);
    }
    const auto bytes = ToBytes(block);

    constexpr u32 Width = 6;
    constexpr u32 Height = 5;
    // Two block columns and two block rows.
    std::vector<u8> data(bytes.size() * 4);
    for (u32 i = 0; i < 4; ++i) {
        std::copy(bytes.begin(), bytes.end(), data.begin() + i * bytes.size());
    }

    std::vector<u8> output(static_cast<size_t>(Width) * Height * 4, 0xcd);
    Tegra::Texture::ETC2::Decompress(data, Format::RGB, Width, Height, 1, output);

    REQUIRE(output.size() == static_cast<size_t>(Width) * Height * 4);
    // The first texel of the image is the texel (0, 0) of the first block and the last one is
    // the texel (1, 0) of the last block. All four blocks are the same and both of those texels
    // sit in the first subblock, so both have the colour of the first base colour.
    REQUIRE(output[0] == 128);
    REQUIRE(output[1] == 247);
    REQUIRE(output[2] == 213);
    const size_t last = (static_cast<size_t>(Width) * Height - 1) * 4;
    REQUIRE(output[last + 0] == 128);
    REQUIRE(output[last + 1] == 247);
    REQUIRE(output[last + 2] == 213);
}

TEST_CASE("ETC2 rejects truncated input", "[video_core][textures][etc2]") {
    const std::vector<u8> data(4, 0);
    std::vector<u8> output(BlockPixels * 4, 0xcd);
    Tegra::Texture::ETC2::Decompress(data, Format::RGB, 4, 4, 1, output);

    for (const u8 byte : output) {
        REQUIRE(byte == 0xcd);
    }
}
