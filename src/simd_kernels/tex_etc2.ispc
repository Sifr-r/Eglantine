// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Multi-ISA ETC2 / EAC block decompressor matching Tegra::Texture::ETC2::Decompress bit-for-bit.

static const uniform int32 MODIFIER_TABLES[8][4] = {
    {-8, -2, 2, 8},
    {-17, -5, 5, 17},
    {-29, -9, 9, 29},
    {-42, -13, 13, 42},
    {-60, -18, 18, 60},
    {-80, -24, 24, 80},
    {-106, -33, 33, 106},
    {-183, -47, 47, 183},
};

static const uniform int32 PUNCHTHROUGH_MODIFIER_TABLES[8][4] = {
    {-8, 0, 0, 8},
    {-17, 0, 0, 17},
    {-29, 0, 0, 29},
    {-42, 0, 0, 42},
    {-60, 0, 0, 60},
    {-80, 0, 0, 80},
    {-106, 0, 0, 106},
    {-183, 0, 0, 183},
};

static const uniform uint32 DISTANCE_TABLE[8] = {3, 6, 11, 16, 23, 32, 41, 64};

static const uniform int32 ALPHA_MODIFIER_TABLES[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14},
    {-3, -7, -10, -13, 2, 6, 9, 12},
    {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12},
    {-3, -6, -8, -12, 2, 5, 7, 11},
    {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10},
    {-3, -5, -8, -11, 2, 4, 7, 10},
    {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},
    {-2, -4, -8, -10, 1, 3, 7, 9},
    {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},
    {-1, -2, -3, -10, 0, 1, 2, 9},
    {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8},
};

static inline uint8 clamp8(int32 value) {
    return (uint8)(value < 0 ? 0 : (value > 255 ? 255 : value));
}

static inline int32 clamp_range(int32 value, int32 low, int32 high) {
    return value < low ? low : (value > high ? high : value);
}

static inline int32 div4_floor(int32 value) {
    return value >= 0 ? value / 4 : -((-value + 3) / 4);
}

static inline uint32 extract_bits(uint64 word, uniform uint32 hi, uniform uint32 lo) {
    return (uint32)((word >> lo) & (((uint64)1 << (hi - lo + 1)) - 1));
}

static inline uint32 extract_bit(uint64 word, uint32 bit_pos) {
    return (uint32)((word >> bit_pos) & 1);
}

static inline uint32 extract_3bits(uint64 word, uniform uint32 lo) {
    return (uint32)((word >> lo) & 7);
}

static inline int32 sign_extend3(uint32 value) {
    return (value & 4) != 0 ? (int32)value - 8 : (int32)value;
}

static inline uint32 extend4(uint32 value) {
    return (value << 4) | value;
}

static inline uint32 extend5(uint32 value) {
    return (value << 3) | (value >> 2);
}

static inline uint32 extend6(uint32 value) {
    return (value << 2) | (value >> 4);
}

static inline uint32 extend7(uint32 value) {
    return (value << 1) | (value >> 6);
}

static inline uint64 load_word_be(const uniform uint8 data[], uint64 offset) {
    uint64 word = 0;
    for (uniform uint32 i = 0; i < 8; ++i) {
        word = (word << 8) | (uint64)data[offset + i];
    }
    return word;
}

static inline uint32 modifier_slot(uint32 index) {
    if (index == 0) {
        return 2;
    }
    if (index == 1) {
        return 3;
    }
    if (index == 2) {
        return 1;
    }
    return 0;
}

static void decode_color(uint64 word, uniform bool punchthrough, uint8 texels[64]) {
    const int32 r = (int32)extract_bits(word, 63, 59);
    const int32 rd = sign_extend3(extract_bits(word, 58, 56));
    const int32 g = (int32)extract_bits(word, 55, 51);
    const int32 gd = sign_extend3(extract_bits(word, 50, 48));
    const int32 b = (int32)extract_bits(word, 47, 43);
    const int32 bd = sign_extend3(extract_bits(word, 42, 40));

    const int32 r2 = r + rd;
    const int32 g2 = g + gd;
    const int32 b2 = b + bd;

    const bool bit33 = extract_bits(word, 33, 33) != 0;
    const bool opaque = !punchthrough || bit33;

    // Modes: 0 = Individual, 1 = Differential, 2 = T, 3 = H, 4 = Planar
    uint32 mode = 1;
    if (!punchthrough && !bit33) {
        mode = 0;
    } else if (r2 < 0 || r2 > 31) {
        mode = 2;
    } else if (g2 < 0 || g2 > 31) {
        mode = 3;
    } else if (b2 < 0 || b2 > 31) {
        mode = 4;
    }

    if (mode == 0 || mode == 1) {
        uint32 b1r, b1g, b1b, b2r, b2g, b2b;
        if (mode == 0) {
            b1r = extend4(extract_bits(word, 63, 60));
            b1g = extend4(extract_bits(word, 55, 52));
            b1b = extend4(extract_bits(word, 47, 44));
            b2r = extend4(extract_bits(word, 59, 56));
            b2g = extend4(extract_bits(word, 51, 48));
            b2b = extend4(extract_bits(word, 43, 40));
        } else {
            b1r = extend5((uint32)r);
            b1g = extend5((uint32)g);
            b1b = extend5((uint32)b);
            b2r = extend5((uint32)r2);
            b2g = extend5((uint32)g2);
            b2b = extend5((uint32)b2);
        }

        const uint32 table1 = extract_bits(word, 39, 37);
        const uint32 table2 = extract_bits(word, 36, 34);
        const bool flip = extract_bits(word, 32, 32) != 0;

        for (uniform uint32 y = 0; y < 4; ++y) {
            for (uniform uint32 x = 0; x < 4; ++x) {
                const bool second = flip ? (y >= 2) : (x >= 2);
                const uint32 br = second ? b2r : b1r;
                const uint32 bg = second ? b2g : b1g;
                const uint32 bb = second ? b2b : b1b;
                const uint32 tbl = second ? table2 : table1;

                const uniform uint32 pidx = x * 4 + y;
                const uint32 idx = (extract_bit(word, 16 + pidx) << 1) | extract_bit(word, pidx);
                const uint32 slot = modifier_slot(idx);
                const int32 mod = opaque ? MODIFIER_TABLES[tbl][slot]
                                         : PUNCHTHROUGH_MODIFIER_TABLES[tbl][slot];

                uint8 cr = clamp8((int32)br + mod);
                uint8 cg = clamp8((int32)bg + mod);
                uint8 cb = clamp8((int32)bb + mod);
                uint8 ca = 255;
                if (!opaque && idx == 2) {
                    cr = 0;
                    cg = 0;
                    cb = 0;
                    ca = 0;
                }

                const uniform uint32 dst = (y * 4 + x) * 4;
                texels[dst + 0] = cr;
                texels[dst + 1] = cg;
                texels[dst + 2] = cb;
                texels[dst + 3] = ca;
            }
        }
        return;
    }

    if (mode == 2 || mode == 3) {
        uint32 b1r, b1g, b1b, b2r, b2g, b2b, dist_idx;
        if (mode == 2) {
            const uint32 red = (extract_bits(word, 60, 59) << 2) | extract_bits(word, 57, 56);
            b1r = extend4(red);
            b1g = extend4(extract_bits(word, 55, 52));
            b1b = extend4(extract_bits(word, 51, 48));
            b2r = extend4(extract_bits(word, 47, 44));
            b2g = extend4(extract_bits(word, 43, 40));
            b2b = extend4(extract_bits(word, 39, 36));
            dist_idx = (extract_bits(word, 35, 34) << 1) | extract_bits(word, 32, 32);
        } else {
            const uint32 red = extract_bits(word, 62, 59);
            const uint32 green = (extract_bits(word, 58, 56) << 1) | extract_bits(word, 52, 52);
            const uint32 blue = (extract_bits(word, 51, 51) << 3) | extract_bits(word, 49, 47);
            const uint32 red2 = extract_bits(word, 46, 43);
            const uint32 green2 = extract_bits(word, 42, 39);
            const uint32 blue2 = extract_bits(word, 38, 35);
            b1r = extend4(red);
            b1g = extend4(green);
            b1b = extend4(blue);
            b2r = extend4(red2);
            b2g = extend4(green2);
            b2b = extend4(blue2);
            const uint32 c1 = (red << 16) | (green << 8) | blue;
            const uint32 c2 = (red2 << 16) | (green2 << 8) | blue2;
            dist_idx = (extract_bits(word, 34, 34) << 2) |
                       (extract_bits(word, 32, 32) << 1) | (c1 >= c2 ? 1u : 0u);
        }

        const int32 dist = (int32)DISTANCE_TABLE[dist_idx];
        uint8 pr[4], pg[4], pb[4];
        if (mode == 2) {
            pr[0] = (uint8)b1r;
            pg[0] = (uint8)b1g;
            pb[0] = (uint8)b1b;
            pr[1] = clamp8((int32)b2r + dist);
            pg[1] = clamp8((int32)b2g + dist);
            pb[1] = clamp8((int32)b2b + dist);
            pr[2] = (uint8)b2r;
            pg[2] = (uint8)b2g;
            pb[2] = (uint8)b2b;
            pr[3] = clamp8((int32)b2r - dist);
            pg[3] = clamp8((int32)b2g - dist);
            pb[3] = clamp8((int32)b2b - dist);
        } else {
            pr[0] = clamp8((int32)b1r + dist);
            pg[0] = clamp8((int32)b1g + dist);
            pb[0] = clamp8((int32)b1b + dist);
            pr[1] = clamp8((int32)b1r - dist);
            pg[1] = clamp8((int32)b1g - dist);
            pb[1] = clamp8((int32)b1b - dist);
            pr[2] = clamp8((int32)b2r + dist);
            pg[2] = clamp8((int32)b2g + dist);
            pb[2] = clamp8((int32)b2b + dist);
            pr[3] = clamp8((int32)b2r - dist);
            pg[3] = clamp8((int32)b2g - dist);
            pb[3] = clamp8((int32)b2b - dist);
        }

        for (uniform uint32 y = 0; y < 4; ++y) {
            for (uniform uint32 x = 0; x < 4; ++x) {
                const uniform uint32 pidx = x * 4 + y;
                const uint32 idx = (extract_bit(word, 16 + pidx) << 1) | extract_bit(word, pidx);
                uint8 cr = pr[idx];
                uint8 cg = pg[idx];
                uint8 cb = pb[idx];
                uint8 ca = 255;
                if (!opaque && idx == 2) {
                    cr = 0;
                    cg = 0;
                    cb = 0;
                    ca = 0;
                }
                const uniform uint32 dst = (y * 4 + x) * 4;
                texels[dst + 0] = cr;
                texels[dst + 1] = cg;
                texels[dst + 2] = cb;
                texels[dst + 3] = ca;
            }
        }
        return;
    }

    // Planar mode
    const int32 red = (int32)extend6(extract_bits(word, 62, 57));
    const int32 green = (int32)extend7((extract_bits(word, 56, 56) << 6) | extract_bits(word, 54, 49));
    const int32 blue = (int32)extend6((extract_bits(word, 48, 48) << 5) |
                                      (extract_bits(word, 44, 43) << 3) |
                                      extract_bits(word, 41, 39));
    const int32 red_h = (int32)extend6((extract_bits(word, 38, 34) << 1) | extract_bits(word, 32, 32));
    const int32 green_h = (int32)extend7(extract_bits(word, 31, 25));
    const int32 blue_h = (int32)extend6(extract_bits(word, 24, 19));
    const int32 red_v = (int32)extend6(extract_bits(word, 18, 13));
    const int32 green_v = (int32)extend7(extract_bits(word, 12, 6));
    const int32 blue_v = (int32)extend6(extract_bits(word, 5, 0));

    for (uniform uint32 y = 0; y < 4; ++y) {
        const uniform int32 cy = (uniform int32)y;
        for (uniform uint32 x = 0; x < 4; ++x) {
            const uniform int32 cx = (uniform int32)x;
            const uniform uint32 dst = (y * 4 + x) * 4;
            texels[dst + 0] = clamp8(div4_floor(cx * (red_h - red) + cy * (red_v - red) + 4 * red + 2));
            texels[dst + 1] = clamp8(div4_floor(cx * (green_h - green) + cy * (green_v - green) + 4 * green + 2));
            texels[dst + 2] = clamp8(div4_floor(cx * (blue_h - blue) + cy * (blue_v - blue) + 4 * blue + 2));
            texels[dst + 3] = 255;
        }
    }
}

static void decode_alpha(uint64 word, uint8 alpha[16]) {
    const int32 base = (int32)extract_bits(word, 63, 56);
    const int32 multiplier = (int32)extract_bits(word, 55, 52);
    const uint32 table = extract_bits(word, 51, 48);

    for (uniform uint32 p = 0; p < 16; ++p) {
        const uint32 index = extract_3bits(word, 45 - 3 * p);
        const int32 modifier = ALPHA_MODIFIER_TABLES[table][index];
        alpha[p] = clamp8(base + modifier * multiplier);
    }
}

static void decode_eac(uint64 word, uniform bool is_signed, uint8 out[16]) {
    int32 base = (int32)extract_bits(word, 63, 56);
    if (is_signed) {
        if (base > 127) {
            base -= 256;
        }
        if (base < -127) {
            base = -127;
        }
    }
    const int32 multiplier = (int32)extract_bits(word, 55, 52);
    const uint32 table = extract_bits(word, 51, 48);

    for (uniform uint32 p = 0; p < 16; ++p) {
        const uint32 index = extract_3bits(word, 45 - 3 * p);
        const int32 modifier = ALPHA_MODIFIER_TABLES[table][index];
        const int32 scaled = multiplier == 0 ? modifier : modifier * multiplier * 8;
        const int32 value = is_signed ? (base * 8 + scaled) : (base * 8 + 4 + scaled);
        const int32 clamped = is_signed ? clamp_range(value, -1023, 1023)
                                        : clamp_range(value, 0, 2047);
        out[p] = (uint8)(clamped >> 3);
    }
}

export void etc2_decompress(const uniform uint8 data[], uniform int32 format,
                            uniform uint32 width, uniform uint32 height,
                            uniform uint32 depth, uniform uint8 output[]) {
    // Format enum mirrors Tegra::Texture::ETC2::Format:
    // 0: RGB, 1: RGB_PTA, 2: RGBA, 3: R11, 4: R11_SNORM, 5: R11G11, 6: R11G11_SNORM
    const uniform uint32 columns = (width + 3) / 4;
    const uniform uint32 rows = (height + 3) / 4;
    const uniform uint32 stride = (format == 3 || format == 4) ? 1u
                                  : ((format == 5 || format == 6) ? 2u : 4u);
    const uniform uint64 block_bytes = (format == 2 || format == 5 || format == 6) ? 16ull : 8ull;
    const uniform bool is_signed = (format == 4 || format == 6);

    for (uniform uint32 z = 0; z < depth; ++z) {
        const uniform uint64 slice_in_base = (uint64)z * rows * columns * block_bytes;
        const uniform uint64 slice_out_base = (uint64)z * width * height * stride;

        for (uniform uint32 by = 0; by < rows; ++by) {
            const uniform uint32 y0 = by * 4;
            const uniform uint32 block_h = min(4u, height - y0);
            const uniform uint64 row_in_base = slice_in_base + (uint64)by * columns * block_bytes;

            foreach (bx = 0 ... columns) {
                const uint32 x0 = bx * 4;
                const uint32 block_w = min(4u, width - x0);
                const uint64 in_off = row_in_base + (uint64)bx * block_bytes;

                if (format == 0 || format == 1) {
                    uint8 texels[64];
                    decode_color(load_word_be(data, in_off), format == 1, texels);
                    for (uniform uint32 y = 0; y < block_h; ++y) {
                        const uint64 dst_row = slice_out_base + ((uint64)(y0 + y) * width + x0) * 4;
                        for (uint32 x = 0; x < block_w; ++x) {
                            const uint64 dst = dst_row + (uint64)x * 4;
                            const uint32 src = (y * 4 + x) * 4;
                            output[dst + 0] = texels[src + 0];
                            output[dst + 1] = texels[src + 1];
                            output[dst + 2] = texels[src + 2];
                            output[dst + 3] = texels[src + 3];
                        }
                    }
                } else if (format == 2) {
                    uint8 texels[64];
                    uint8 alpha[16];
                    decode_alpha(load_word_be(data, in_off), alpha);
                    decode_color(load_word_be(data, in_off + 8), false, texels);
                    for (uniform uint32 y = 0; y < block_h; ++y) {
                        const uint64 dst_row = slice_out_base + ((uint64)(y0 + y) * width + x0) * 4;
                        for (uint32 x = 0; x < block_w; ++x) {
                            const uint64 dst = dst_row + (uint64)x * 4;
                            const uint32 src = (y * 4 + x) * 4;
                            output[dst + 0] = texels[src + 0];
                            output[dst + 1] = texels[src + 1];
                            output[dst + 2] = texels[src + 2];
                            output[dst + 3] = alpha[x * 4 + y];
                        }
                    }
                } else if (format == 3 || format == 4) {
                    uint8 values[16];
                    decode_eac(load_word_be(data, in_off), is_signed, values);
                    for (uniform uint32 y = 0; y < block_h; ++y) {
                        const uint64 dst_row = slice_out_base + (uint64)(y0 + y) * width + x0;
                        for (uint32 x = 0; x < block_w; ++x) {
                            output[dst_row + x] = values[x * 4 + y];
                        }
                    }
                } else {
                    uint8 red[16];
                    uint8 green[16];
                    decode_eac(load_word_be(data, in_off), is_signed, red);
                    decode_eac(load_word_be(data, in_off + 8), is_signed, green);
                    for (uniform uint32 y = 0; y < block_h; ++y) {
                        const uint64 dst_row = slice_out_base + ((uint64)(y0 + y) * width + x0) * 2;
                        for (uint32 x = 0; x < block_w; ++x) {
                            const uint64 dst = dst_row + (uint64)x * 2;
                            const uint32 pidx = x * 4 + y;
                            output[dst + 0] = red[pidx];
                            output[dst + 1] = green[pidx];
                        }
                    }
                }
            }
        }
    }
}
