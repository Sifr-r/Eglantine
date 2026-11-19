// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Software decoder for the ETC2 and EAC texture compression formats. The layout of the blocks
// and the decoding rules follow the Khronos Data Format Specification, section 22 "ETC2
// Compressed Texture Image Formats".

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>

#include "common/common_types.h"
#include "common/div_ceil.h"
#include "common/logging.h"
#include "video_core/textures/etc2.h"

namespace Tegra::Texture::ETC2 {
namespace {

constexpr u32 BLOCK_WIDTH = 4;
constexpr u32 BLOCK_HEIGHT = 4;
constexpr u32 BLOCK_PIXELS = BLOCK_WIDTH * BLOCK_HEIGHT;

/// A colour with one byte per channel.
struct Color {
    u8 r;
    u8 g;
    u8 b;
};

constexpr u8 Clamp8(s32 value) {
    return static_cast<u8>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

constexpr s32 Clamp(s32 value, s32 low, s32 high) {
    return value < low ? low : (value > high ? high : value);
}

/// Divides by four, rounding towards negative infinity as the planar mode requires.
constexpr s32 Div4(s32 value) {
    return value >= 0 ? value / 4 : -((-value + 3) / 4);
}

/// Extracts the bits `hi` down to `lo`, both inclusive.
constexpr u32 Bits(u64 word, u32 hi, u32 lo) {
    return static_cast<u32>((word >> lo) & ((u64{1} << (hi - lo + 1)) - 1));
}

/// Sign extends the three bit two's complement delta of the differential mode.
constexpr s32 SignExtend3(u32 value) {
    return (value & 4) != 0 ? static_cast<s32>(value) - 8 : static_cast<s32>(value);
}

/// The base colours are stored with fewer than eight bits per channel and are extended by
/// replicating their most significant bits.
constexpr u8 Extend4(u32 value) {
    return static_cast<u8>((value << 4) | value);
}

constexpr u8 Extend5(u32 value) {
    return static_cast<u8>((value << 3) | (value >> 2));
}

constexpr u8 Extend6(u32 value) {
    return static_cast<u8>((value << 2) | (value >> 4));
}

constexpr u8 Extend7(u32 value) {
    return static_cast<u8>((value << 1) | (value >> 6));
}

/// The eight intensity modifier tables of the 'individual' and 'differential' modes. The table
/// codeword selects one of them, the pixel index then picks one of its four entries.
constexpr std::array<std::array<s32, 4>, 8> ModifierTables{{
    {{-8, -2, 2, 8}},
    {{-17, -5, 5, 17}},
    {{-29, -9, 9, 29}},
    {{-42, -13, 13, 42}},
    {{-60, -18, 18, 60}},
    {{-80, -24, 24, 80}},
    {{-106, -33, 33, 106}},
    {{-183, -47, 47, 183}},
}};

/// The same tables with the two inner entries zeroed. A punchthrough block that is not fully
/// opaque reserves one pixel index for transparency and must not shift the colour with it.
constexpr std::array<std::array<s32, 4>, 8> PunchthroughModifierTables{{
    {{-8, 0, 0, 8}},
    {{-17, 0, 0, 17}},
    {{-29, 0, 0, 29}},
    {{-42, 0, 0, 42}},
    {{-60, 0, 0, 60}},
    {{-80, 0, 0, 80}},
    {{-106, 0, 0, 106}},
    {{-183, 0, 0, 183}},
}};

/// The distances the 'T' and 'H' modes use to derive their paint colours.
constexpr std::array<u32, 8> DistanceTable{3, 6, 11, 16, 23, 32, 41, 64};

/// The sixteen modifier tables of the alpha part of RGBA ETC2 and of the EAC formats.
constexpr std::array<std::array<s32, 8>, 16> AlphaModifierTables{{
    {{-3, -6, -9, -15, 2, 5, 8, 14}},
    {{-3, -7, -10, -13, 2, 6, 9, 12}},
    {{-2, -5, -8, -13, 1, 4, 7, 12}},
    {{-2, -4, -6, -13, 1, 3, 5, 12}},
    {{-3, -6, -8, -12, 2, 5, 7, 11}},
    {{-3, -7, -9, -11, 2, 6, 8, 10}},
    {{-4, -7, -8, -11, 3, 6, 7, 10}},
    {{-3, -5, -8, -11, 2, 4, 7, 10}},
    {{-2, -6, -8, -10, 1, 5, 7, 9}},
    {{-2, -5, -8, -10, 1, 4, 7, 9}},
    {{-2, -4, -8, -10, 1, 3, 7, 9}},
    {{-2, -5, -7, -10, 1, 4, 6, 9}},
    {{-3, -4, -7, -10, 2, 3, 6, 9}},
    {{-1, -2, -3, -10, 0, 1, 2, 9}},
    {{-4, -6, -8, -9, 3, 5, 7, 8}},
    {{-3, -5, -7, -9, 2, 4, 6, 8}},
}};

/// Loads the eight bytes of a block as the big endian 64 bit word the specification describes.
u64 LoadWord(std::span<const u8> data) {
    u64 word = 0;
    for (u32 i = 0; i < 8; ++i) {
        word = (word << 8) | data[i];
    }
    return word;
}

/// The specification numbers the texels of a block column major, so the texel at (0, 0) has
/// index zero and the texel at (3, 3) has index fifteen.
constexpr u32 PixelIndex(u32 x, u32 y) {
    return x * BLOCK_HEIGHT + y;
}

/// The two bit index of a texel. Its least significant bit lives in the lower half of the block
/// and its most significant bit in the upper half.
constexpr u32 PixelIndexBits(u64 word, u32 x, u32 y) {
    const u32 index = PixelIndex(x, y);
    return (Bits(word, 16 + index, 16 + index) << 1) | Bits(word, index, index);
}

/// The pixel index does not select a modifier table entry directly: it picks between the small
/// and the large positive and negative modifier of the selected table.
constexpr u32 ModifierSlot(u32 index) {
    switch (index) {
    case 0:
        return 2;
    case 1:
        return 3;
    case 2:
        return 1;
    default:
        return 0;
    }
}

/// Adds a distance to every channel of a colour.
constexpr Color AddDistance(Color color, s32 distance) {
    return {Clamp8(color.r + distance), Clamp8(color.g + distance), Clamp8(color.b + distance)};
}

/// The value a colour is compared by in the 'H' mode.
constexpr u32 ColorValue(u32 r, u32 g, u32 b) {
    return (r << 16) | (g << 8) | b;
}

/// Decodes the colour part of one block into sixteen RGBA8 texels.
void DecodeColor(u64 word, bool punchthrough, std::array<u8, BLOCK_PIXELS * 4>& out) {
    const s32 r = Bits(word, 63, 59);
    const s32 rd = SignExtend3(Bits(word, 58, 56));
    const s32 g = Bits(word, 55, 51);
    const s32 gd = SignExtend3(Bits(word, 50, 48));
    const s32 b = Bits(word, 47, 43);
    const s32 bd = SignExtend3(Bits(word, 42, 40));

    // The differential mode adds a signed delta to the five bit base colour. A sum that leaves
    // the five bit range makes the block invalid for that mode; it is then one of the other
    // modes instead, the red overflow taking precedence over the green one and the green one
    // over the blue one.
    const s32 r2 = r + rd;
    const s32 g2 = g + gd;
    const s32 b2 = b + bd;

    // In the punchthrough format bit 33 says whether the whole block is opaque, in the opaque
    // colour format it selects the individual or the differential mode.
    const bool opaque = !punchthrough || Bits(word, 33, 33) != 0;

    enum class Mode { Individual, Differential, T, H, Planar };
    const Mode mode = [&] {
        if (!punchthrough && Bits(word, 33, 33) == 0) {
            return Mode::Individual;
        }
        if (r2 < 0 || r2 > 31) {
            return Mode::T;
        }
        if (g2 < 0 || g2 > 31) {
            return Mode::H;
        }
        if (b2 < 0 || b2 > 31) {
            return Mode::Planar;
        }
        return Mode::Differential;
    }();

    if (mode == Mode::Individual || mode == Mode::Differential) {
        Color base1;
        Color base2;
        if (mode == Mode::Individual) {
            base1 = {Extend4(Bits(word, 63, 60)), Extend4(Bits(word, 55, 52)),
                     Extend4(Bits(word, 47, 44))};
            base2 = {Extend4(Bits(word, 59, 56)), Extend4(Bits(word, 51, 48)),
                     Extend4(Bits(word, 43, 40))};
        } else {
            base1 = {Extend5(r), Extend5(g), Extend5(b)};
            base2 = {Extend5(r2), Extend5(g2), Extend5(b2)};
        }

        const u32 table1 = Bits(word, 39, 37);
        const u32 table2 = Bits(word, 36, 34);
        const auto& tables = opaque ? ModifierTables : PunchthroughModifierTables;

        // The flip bit decides whether the two subblocks sit side by side or on top of each
        // other. The first subblock holds the left columns or the top rows.
        const bool flip = Bits(word, 32, 32) != 0;

        for (u32 y = 0; y < BLOCK_HEIGHT; ++y) {
            for (u32 x = 0; x < BLOCK_WIDTH; ++x) {
                const u32 subblock = flip ? (y >= 2 ? 1 : 0) : (x >= 2 ? 1 : 0);
                const Color base = subblock == 0 ? base1 : base2;
                const auto& table = tables[subblock == 0 ? table1 : table2];
                const u32 index = PixelIndexBits(word, x, y);

                Color color = AddDistance(base, table[ModifierSlot(index)]);
                u8 alpha = 255;
                if (!opaque && index == 2) {
                    // The reserved pixel index is fully transparent, and its colour is zeroed.
                    color = {0, 0, 0};
                    alpha = 0;
                }

                const size_t offset = (y * BLOCK_WIDTH + x) * 4;
                out[offset + 0] = color.r;
                out[offset + 1] = color.g;
                out[offset + 2] = color.b;
                out[offset + 3] = alpha;
            }
        }
        return;
    }

    if (mode == Mode::T || mode == Mode::H) {
        Color base1;
        Color base2;
        u32 distance_index;
        if (mode == Mode::T) {
            // The red channel of the first base colour is split over two places in the block.
            const u32 red = (Bits(word, 60, 59) << 2) | Bits(word, 57, 56);
            base1 = {Extend4(red), Extend4(Bits(word, 55, 52)), Extend4(Bits(word, 51, 48))};
            base2 = {Extend4(Bits(word, 47, 44)), Extend4(Bits(word, 43, 40)),
                     Extend4(Bits(word, 39, 36))};
            // The distance index combines the two bit 'da' with the single bit 'db'.
            distance_index = (Bits(word, 35, 34) << 1) | Bits(word, 32, 32);
        } else {
            const u32 red = Bits(word, 62, 59);
            const u32 green = (Bits(word, 58, 56) << 1) | Bits(word, 52, 52);
            const u32 blue = (Bits(word, 51, 51) << 3) | Bits(word, 49, 47);
            const u32 red2 = Bits(word, 46, 43);
            const u32 green2 = Bits(word, 42, 39);
            const u32 blue2 = Bits(word, 38, 35);
            base1 = {Extend4(red), Extend4(green), Extend4(blue)};
            base2 = {Extend4(red2), Extend4(green2), Extend4(blue2)};
            // The least significant bit of the distance index is not stored, it compares the two
            // base colours instead.
            distance_index = (Bits(word, 34, 34) << 2) | (Bits(word, 32, 32) << 1) |
                             (ColorValue(red, green, blue) >= ColorValue(red2, green2, blue2) ? 1 : 0);
        }

        const s32 distance = static_cast<s32>(DistanceTable[distance_index]);
        const std::array<Color, 4> paint{mode == Mode::T
                                             ? std::array<Color, 4>{base1, AddDistance(base2, distance),
                                                                   base2, AddDistance(base2, -distance)}
                                             : std::array<Color, 4>{AddDistance(base1, distance),
                                                                   AddDistance(base1, -distance),
                                                                   AddDistance(base2, distance),
                                                                   AddDistance(base2, -distance)}};

        for (u32 y = 0; y < BLOCK_HEIGHT; ++y) {
            for (u32 x = 0; x < BLOCK_WIDTH; ++x) {
                const u32 index = PixelIndexBits(word, x, y);

                Color color = paint[index];
                u8 alpha = 255;
                if (!opaque && index == 2) {
                    color = {0, 0, 0};
                    alpha = 0;
                }

                const size_t offset = (y * BLOCK_WIDTH + x) * 4;
                out[offset + 0] = color.r;
                out[offset + 1] = color.g;
                out[offset + 2] = color.b;
                out[offset + 3] = alpha;
            }
        }
        return;
    }

    // The planar mode interpolates between three base colours.
    const s32 red = Extend6(Bits(word, 62, 57));
    const s32 green = Extend7((Bits(word, 56, 56) << 6) | Bits(word, 54, 49));
    const s32 blue = Extend6((Bits(word, 48, 48) << 5) | (Bits(word, 44, 43) << 3) |
                             Bits(word, 41, 39));
    const s32 red_h = Extend6((Bits(word, 38, 34) << 1) | Bits(word, 32, 32));
    const s32 green_h = Extend7(Bits(word, 31, 25));
    const s32 blue_h = Extend6(Bits(word, 24, 19));
    const s32 red_v = Extend6(Bits(word, 18, 13));
    const s32 green_v = Extend7(Bits(word, 12, 6));
    const s32 blue_v = Extend6(Bits(word, 5, 0));

    for (u32 y = 0; y < BLOCK_HEIGHT; ++y) {
        for (u32 x = 0; x < BLOCK_WIDTH; ++x) {
            const s32 cx = static_cast<s32>(x);
            const s32 cy = static_cast<s32>(y);
            const u8 pr = Clamp8(Div4(cx * (red_h - red) + cy * (red_v - red) + 4 * red + 2));
            const u8 pg = Clamp8(Div4(cx * (green_h - green) + cy * (green_v - green) + 4 * green + 2));
            const u8 pb = Clamp8(Div4(cx * (blue_h - blue) + cy * (blue_v - blue) + 4 * blue + 2));

            const size_t offset = (y * BLOCK_WIDTH + x) * 4;
            out[offset + 0] = pr;
            out[offset + 1] = pg;
            out[offset + 2] = pb;
            out[offset + 3] = 255;
        }
    }
}

/// Decodes the alpha part of one RGBA ETC2 block into sixteen alpha values.
void DecodeAlpha(u64 word, std::array<u8, BLOCK_PIXELS>& out) {
    const s32 base = Bits(word, 63, 56);
    const s32 multiplier = Bits(word, 55, 52);
    const u32 table = Bits(word, 51, 48);

    for (u32 p = 0; p < BLOCK_PIXELS; ++p) {
        // The alpha indices are stored in the opposite order to the colour ones: the index of
        // the first texel sits in the three most significant bits of the index area.
        const u32 index = Bits(word, 47 - 3 * p, 45 - 3 * p);
        const s32 modifier = AlphaModifierTables[table][index];
        out[p] = Clamp8(base + modifier * multiplier);
    }
}

/// Decodes one EAC block into sixteen single channel values.
void DecodeEac(u64 word, bool is_signed, std::array<u8, BLOCK_PIXELS>& out) {
    s32 base = Bits(word, 63, 56);
    if (is_signed) {
        // The base codeword is a two's complement value; the forbidden value -128 is read as -127.
        if (base > 127) {
            base -= 256;
        }
        if (base < -127) {
            base = -127;
        }
    }
    const s32 multiplier = Bits(word, 55, 52);
    const u32 table = Bits(word, 51, 48);

    for (u32 p = 0; p < BLOCK_PIXELS; ++p) {
        const u32 index = Bits(word, 47 - 3 * p, 45 - 3 * p);
        const s32 modifier = AlphaModifierTables[table][index];
        // A multiplier of zero means an eighth, which leaves the modifier unscaled.
        const s32 scaled = multiplier == 0 ? modifier : modifier * multiplier * 8;
        const s32 value = is_signed ? base * 8 + scaled : base * 8 + 4 + scaled;
        const s32 clamped = is_signed ? Clamp(value, -1023, 1023) : Clamp(value, 0, 2047);
        // The eleven bit result is truncated to the eight bits the host format keeps.
        out[p] = static_cast<u8>(clamped >> 3);
    }
}

/// Decodes one block into sixteen row-major texels.
void DecodeBlock(Format format, std::span<const u8> block,
                 std::array<u8, BLOCK_PIXELS * 4>& out) {
    switch (format) {
    case Format::RGB:
    case Format::RGB_PTA:
        DecodeColor(LoadWord(block), format == Format::RGB_PTA, out);
        break;
    case Format::RGBA: {
        // The alpha half of the block comes first, the colour half second.
        DecodeColor(LoadWord(block.subspan(8, 8)), false, out);
        std::array<u8, BLOCK_PIXELS> alpha;
        DecodeAlpha(LoadWord(block.subspan(0, 8)), alpha);
        for (u32 y = 0; y < BLOCK_HEIGHT; ++y) {
            for (u32 x = 0; x < BLOCK_WIDTH; ++x) {
                out[(y * BLOCK_WIDTH + x) * 4 + 3] = alpha[PixelIndex(x, y)];
            }
        }
        break;
    }
    case Format::R11:
    case Format::R11_SNORM: {
        std::array<u8, BLOCK_PIXELS> values;
        DecodeEac(LoadWord(block), format == Format::R11_SNORM, values);
        for (u32 y = 0; y < BLOCK_HEIGHT; ++y) {
            for (u32 x = 0; x < BLOCK_WIDTH; ++x) {
                out[y * BLOCK_WIDTH + x] = values[PixelIndex(x, y)];
            }
        }
        break;
    }
    case Format::R11G11:
    case Format::R11G11_SNORM: {
        // The red channel comes first, the green channel second.
        std::array<u8, BLOCK_PIXELS> red;
        std::array<u8, BLOCK_PIXELS> green;
        DecodeEac(LoadWord(block.subspan(0, 8)), format == Format::R11G11_SNORM, red);
        DecodeEac(LoadWord(block.subspan(8, 8)), format == Format::R11G11_SNORM, green);
        for (u32 y = 0; y < BLOCK_HEIGHT; ++y) {
            for (u32 x = 0; x < BLOCK_WIDTH; ++x) {
                const u32 dst = (y * BLOCK_WIDTH + x) * 2;
                const u32 src = PixelIndex(x, y);
                out[dst + 0] = red[src];
                out[dst + 1] = green[src];
            }
        }
        break;
    }
    }
}

} // Anonymous namespace

u32 BytesPerTexel(Format format) {
    switch (format) {
    case Format::R11:
    case Format::R11_SNORM:
        return 1;
    case Format::R11G11:
    case Format::R11G11_SNORM:
        return 2;
    default:
        return 4;
    }
}

void Decompress(std::span<const u8> data, Format format, u32 width, u32 height, u32 depth,
                std::span<u8> output) {
    const u32 stride = BytesPerTexel(format);
    const u32 block_bytes = (format == Format::RGB || format == Format::RGB_PTA ||
                              format == Format::R11 || format == Format::R11_SNORM)
                                 ? 8
                                 : 16;

    const u32 columns = Common::DivCeil(width, BLOCK_WIDTH);
    const u32 rows = Common::DivCeil(height, BLOCK_HEIGHT);
    const size_t blocks = static_cast<size_t>(columns) * rows * depth;
    const size_t required_input = blocks * block_bytes;
    if (data.size() < required_input) {
        LOG_ERROR(HW_GPU, "ETC2: truncated input, got {} bytes but need {}", data.size(),
                  required_input);
        return;
    }

    const size_t required_output = static_cast<size_t>(width) * height * depth * stride;
    if (output.size() < required_output) {
        LOG_ERROR(HW_GPU, "ETC2: output buffer too small, got {} bytes but need {}", output.size(),
                  required_output);
        return;
    }

    size_t input_offset = 0;
    std::array<u8, BLOCK_PIXELS * 4> texels;
    for (u32 z = 0; z < depth; ++z) {
        const size_t slice_offset = static_cast<size_t>(z) * width * height * stride;
        for (u32 by = 0; by < rows; ++by) {
            const u32 y0 = by * BLOCK_HEIGHT;
            const u32 block_height = (std::min)(BLOCK_HEIGHT, height - y0);
            for (u32 bx = 0; bx < columns; ++bx) {
                const u32 x0 = bx * BLOCK_WIDTH;
                const u32 block_width = (std::min)(BLOCK_WIDTH, width - x0);

                DecodeBlock(format, data.subspan(input_offset, block_bytes), texels);
                input_offset += block_bytes;

                // Only the texels inside the image are written, the padding of the last block
                // row and column is dropped.
                for (u32 y = 0; y < block_height; ++y) {
                    for (u32 x = 0; x < block_width; ++x) {
                        const size_t destination =
                            slice_offset + (static_cast<size_t>(y0 + y) * width + x0 + x) * stride;
                        std::memcpy(output.data() + destination,
                                    &texels[(y * BLOCK_WIDTH + x) * stride], stride);
                    }
                }
            }
        }
    }
}

} // namespace Tegra::Texture::ETC2
