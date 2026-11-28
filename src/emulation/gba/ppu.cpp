// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "emulation/gba/ppu.h"

#include <algorithm>

namespace Emulation::Gba {
namespace {

constexpr std::int32_t SignExtend28(std::int32_t v) {
    return (v << 4) >> 4;
}

bool InWinH(std::uint16_t winh, std::size_t x) {
    const std::size_t x1 = (winh >> 8) & 0xFFu;
    const std::size_t x2 = winh & 0xFFu;
    return (x1 <= x2) ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
}

bool InWinV(std::uint16_t winv, std::size_t y) {
    const std::size_t y1 = (winv >> 8) & 0xFFu;
    const std::size_t y2 = winv & 0xFFu;
    return (y1 <= y2) ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
}

std::uint16_t AlphaBlend(std::uint16_t a, std::uint16_t b, std::uint16_t eva, std::uint16_t evb) {
    const std::uint16_t r = (std::min<std::uint16_t>)(
        static_cast<std::uint16_t>(((a & 0x1Fu) * eva + (b & 0x1Fu) * evb) >> 4), 31u);
    const std::uint16_t g = (std::min<std::uint16_t>)(
        static_cast<std::uint16_t>((((a >> 5) & 0x1Fu) * eva + ((b >> 5) & 0x1Fu) * evb) >> 4),
        31u);
    const std::uint16_t bl = (std::min<std::uint16_t>)(
        static_cast<std::uint16_t>((((a >> 10) & 0x1Fu) * eva + ((b >> 10) & 0x1Fu) * evb) >> 4),
        31u);
    return static_cast<std::uint16_t>(r | (g << 5) | (bl << 10));
}

std::uint16_t Brighten(std::uint16_t c, std::uint16_t evy) {
    const std::uint16_t r = c & 0x1Fu;
    const std::uint16_t g = (c >> 5) & 0x1Fu;
    const std::uint16_t b = (c >> 10) & 0x1Fu;
    const std::uint16_t nr = static_cast<std::uint16_t>(r + (((31u - r) * evy) >> 4));
    const std::uint16_t ng = static_cast<std::uint16_t>(g + (((31u - g) * evy) >> 4));
    const std::uint16_t nb = static_cast<std::uint16_t>(b + (((31u - b) * evy) >> 4));
    return static_cast<std::uint16_t>(nr | (ng << 5) | (nb << 10));
}

std::uint16_t Darken(std::uint16_t c, std::uint16_t evy) {
    const std::uint16_t r = c & 0x1Fu;
    const std::uint16_t g = (c >> 5) & 0x1Fu;
    const std::uint16_t b = (c >> 10) & 0x1Fu;
    const std::uint16_t nr = static_cast<std::uint16_t>(r - ((r * evy) >> 4));
    const std::uint16_t ng = static_cast<std::uint16_t>(g - ((g * evy) >> 4));
    const std::uint16_t nb = static_cast<std::uint16_t>(b - ((b * evy) >> 4));
    return static_cast<std::uint16_t>(nr | (ng << 5) | (nb << 10));
}

std::uint16_t BlendRgb555(std::uint16_t a, std::uint16_t b) {
    const std::uint16_t r = static_cast<std::uint16_t>(((a & 0x1Fu) + (b & 0x1Fu)) >> 1);
    const std::uint16_t g = static_cast<std::uint16_t>((((a >> 5) & 0x1Fu) + ((b >> 5) & 0x1Fu)) >> 1);
    const std::uint16_t bl = static_cast<std::uint16_t>((((a >> 10) & 0x1Fu) + ((b >> 10) & 0x1Fu)) >> 1);
    return static_cast<std::uint16_t>(r | (g << 5) | (bl << 10));
}

std::array<std::uint32_t, 32768> BuildRgb555Lut(bool color_correction) {
    std::array<std::uint32_t, 32768> lut{};
    for (std::uint32_t rgb555 = 0; rgb555 < 32768u; ++rgb555) {
        const std::uint32_t r5 = rgb555 & 0x1Fu;
        const std::uint32_t g5 = (rgb555 >> 5) & 0x1Fu;
        const std::uint32_t b5 = (rgb555 >> 10) & 0x1Fu;
        std::uint32_t r8 = 0;
        std::uint32_t g8 = 0;
        std::uint32_t b8 = 0;
        if (color_correction) {
            r8 = (std::min<std::uint32_t>)((r5 * 224u + g5 * 20u + b5 * 8u) >> 5, 255u);
            g8 = (std::min<std::uint32_t>)((r5 * 10u + g5 * 204u + b5 * 36u) >> 5, 255u);
            b8 = (std::min<std::uint32_t>)((r5 * 4u + g5 * 28u + b5 * 212u) >> 5, 255u);
        } else {
            r8 = (r5 << 3) | (r5 >> 2);
            g8 = (g5 << 3) | (g5 >> 2);
            b8 = (b5 << 3) | (b5 >> 2);
        }
        lut[rgb555] = 0xFF000000u | (b8 << 16) | (g8 << 8) | r8;
    }
    return lut;
}

const std::array<std::uint32_t, 32768>& GetRawLut() {
    static const auto lut = BuildRgb555Lut(false);
    return lut;
}

const std::array<std::uint32_t, 32768>& GetCorrectedLut() {
    static const auto lut = BuildRgb555Lut(true);
    return lut;
}

std::int32_t FloorMod(std::int32_t a, std::int32_t m) {
    const std::int32_t r = a % m;
    return r < 0 ? (r + m) : r;
}

} // namespace

std::uint32_t Rgb555ToAbgr8888(std::uint16_t rgb555, bool color_correction) {
    const auto& lut = color_correction ? GetCorrectedLut() : GetRawLut();
    return lut[rgb555 & 0x7FFFu];
}

Ppu::Ppu() {
    Reset();
}

void Ppu::Reset() {
    const VideoOptions opts = video_options;
    palette_ram.assign(1024, 0);
    vram.assign(96 * 1024, 0);
    oam.assign(1024, 0);
    dispcnt = 0x0080;
    dispstat = 0;
    vcount = 0;
    bgcnt.fill(0);
    bghofs.fill(0);
    bgvofs.fill(0);
    bg_affine = {BgAffine{}, BgAffine{}};
    winh.fill(0);
    winv.fill(0);
    winin = 0;
    winout = 0;
    mosaic = 0;
    bldcnt = 0;
    bldalpha = 0;
    bldy = 0;
    cycle_in_line = 0;
    in_hblank = false;
    video_options = opts;
    raw_frame.assign(ScreenWidth * ScreenHeight, 0);
    prev_raw_frame.assign(ScreenWidth * ScreenHeight, 0);
    output_frame.assign(ScreenWidth * ScreenHeight, 0xFF000000u);
}

void Ppu::RefreshOutputFrame() {
    const auto& lut = video_options.color_correction ? GetCorrectedLut() : GetRawLut();
    const std::size_t count = ScreenWidth * ScreenHeight;
    const std::uint16_t* raw = raw_frame.data();
    const std::uint16_t* prev = prev_raw_frame.data();
    std::uint32_t* out = output_frame.data();

    if (video_options.interframe_blending) {
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = lut[BlendRgb555(raw[i], prev[i]) & 0x7FFFu];
        }
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = lut[raw[i] & 0x7FFFu];
        }
    }
}

PpuStepEvents Ppu::Step(std::uint32_t cycles) {
    PpuStepEvents ev{};
    cycle_in_line += cycles;

    for (;;) {
        if (!in_hblank) {
            if (cycle_in_line >= HdrawCycles) {
                in_hblank = true;
                dispstat |= static_cast<std::uint16_t>(1u << 1);
                if (vcount < VisibleScanlines) {
                    RenderScanline(vcount);
                    ev.entered_hblank = true;
                }
                if ((dispstat & (1u << 4)) != 0) {
                    ev.irq_mask |= IrqHBlank;
                }
            } else {
                break;
            }
        } else if (cycle_in_line >= HdrawCycles + HblankCycles) {
            cycle_in_line -= HdrawCycles + HblankCycles;
            in_hblank = false;
            dispstat &= static_cast<std::uint16_t>(~(1u << 1));
            if (vcount < VisibleScanlines) {
                for (auto& aff : bg_affine) {
                    aff.internal_x += static_cast<std::int32_t>(aff.pb);
                    aff.internal_y += static_cast<std::int32_t>(aff.pd);
                }
            }
            vcount = static_cast<std::uint16_t>((vcount + 1u) % ScanlinesPerFrame);

            if (vcount == VisibleScanlines) {
                dispstat |= static_cast<std::uint16_t>(1u << 0);
                ev.entered_vblank = true;
                ev.frame_complete = true;
                RefreshOutputFrame();
                std::copy(raw_frame.begin(), raw_frame.end(), prev_raw_frame.begin());
                for (auto& aff : bg_affine) {
                    aff.internal_x = aff.ref_x;
                    aff.internal_y = aff.ref_y;
                }
                if ((dispstat & (1u << 3)) != 0) {
                    ev.irq_mask |= IrqVBlank;
                }
            } else if (vcount == 0) {
                dispstat &= static_cast<std::uint16_t>(~(1u << 0));
                for (auto& aff : bg_affine) {
                    aff.internal_x = aff.ref_x;
                    aff.internal_y = aff.ref_y;
                }
            }

            const std::uint16_t lyc = static_cast<std::uint16_t>((dispstat >> 8) & 0xFFu);
            if (vcount == lyc) {
                dispstat |= static_cast<std::uint16_t>(1u << 2);
                if ((dispstat & (1u << 5)) != 0) {
                    ev.irq_mask |= IrqVCount;
                }
            } else {
                dispstat &= static_cast<std::uint16_t>(~(1u << 2));
            }
        } else {
            break;
        }
    }
    return ev;
}

std::uint16_t Ppu::ReadIoU16(std::uint32_t offset) const {
    switch (offset) {
    case 0x00:
        return dispcnt;
    case 0x04:
        return dispstat;
    case 0x06:
        return vcount;
    case 0x08:
        return bgcnt[0];
    case 0x0A:
        return bgcnt[1];
    case 0x0C:
        return bgcnt[2];
    case 0x0E:
        return bgcnt[3];
    case 0x48:
        return winin;
    case 0x4A:
        return winout;
    case 0x4C:
        return mosaic;
    case 0x50:
        return bldcnt;
    case 0x52:
        return bldalpha;
    case 0x54:
        return bldy;
    default:
        return 0;
    }
}

void Ppu::WriteIoU16(std::uint32_t offset, std::uint16_t val) {
    switch (offset) {
    case 0x00:
        dispcnt = val;
        break;
    case 0x04: {
        dispstat = static_cast<std::uint16_t>((dispstat & 0x0007u) | (val & 0xFF38u));
        const std::uint16_t lyc = static_cast<std::uint16_t>((dispstat >> 8) & 0xFFu);
        if (vcount == lyc) {
            dispstat |= static_cast<std::uint16_t>(1u << 2);
        } else {
            dispstat &= static_cast<std::uint16_t>(~(1u << 2));
        }
        break;
    }
    case 0x08:
        bgcnt[0] = val;
        break;
    case 0x0A:
        bgcnt[1] = val;
        break;
    case 0x0C:
        bgcnt[2] = val;
        break;
    case 0x0E:
        bgcnt[3] = val;
        break;
    case 0x10:
        bghofs[0] = val & 0x1FFu;
        break;
    case 0x12:
        bgvofs[0] = val & 0x1FFu;
        break;
    case 0x14:
        bghofs[1] = val & 0x1FFu;
        break;
    case 0x16:
        bgvofs[1] = val & 0x1FFu;
        break;
    case 0x18:
        bghofs[2] = val & 0x1FFu;
        break;
    case 0x1A:
        bgvofs[2] = val & 0x1FFu;
        break;
    case 0x1C:
        bghofs[3] = val & 0x1FFu;
        break;
    case 0x1E:
        bgvofs[3] = val & 0x1FFu;
        break;
    case 0x20:
        bg_affine[0].pa = static_cast<std::int16_t>(val);
        break;
    case 0x22:
        bg_affine[0].pb = static_cast<std::int16_t>(val);
        break;
    case 0x24:
        bg_affine[0].pc = static_cast<std::int16_t>(val);
        break;
    case 0x26:
        bg_affine[0].pd = static_cast<std::int16_t>(val);
        break;
    case 0x28:
        bg_affine[0].ref_x = (bg_affine[0].ref_x & ~0xFFFF) | static_cast<std::int32_t>(val);
        bg_affine[0].internal_x = bg_affine[0].ref_x;
        break;
    case 0x2A: {
        const std::int32_t hi = SignExtend28((static_cast<std::int32_t>(val) & 0x0FFF) << 16);
        bg_affine[0].ref_x = (bg_affine[0].ref_x & 0xFFFF) | hi;
        bg_affine[0].internal_x = bg_affine[0].ref_x;
        break;
    }
    case 0x2C:
        bg_affine[0].ref_y = (bg_affine[0].ref_y & ~0xFFFF) | static_cast<std::int32_t>(val);
        bg_affine[0].internal_y = bg_affine[0].ref_y;
        break;
    case 0x2E: {
        const std::int32_t hi = SignExtend28((static_cast<std::int32_t>(val) & 0x0FFF) << 16);
        bg_affine[0].ref_y = (bg_affine[0].ref_y & 0xFFFF) | hi;
        bg_affine[0].internal_y = bg_affine[0].ref_y;
        break;
    }
    case 0x30:
        bg_affine[1].pa = static_cast<std::int16_t>(val);
        break;
    case 0x32:
        bg_affine[1].pb = static_cast<std::int16_t>(val);
        break;
    case 0x34:
        bg_affine[1].pc = static_cast<std::int16_t>(val);
        break;
    case 0x36:
        bg_affine[1].pd = static_cast<std::int16_t>(val);
        break;
    case 0x38:
        bg_affine[1].ref_x = (bg_affine[1].ref_x & ~0xFFFF) | static_cast<std::int32_t>(val);
        bg_affine[1].internal_x = bg_affine[1].ref_x;
        break;
    case 0x3A: {
        const std::int32_t hi = SignExtend28((static_cast<std::int32_t>(val) & 0x0FFF) << 16);
        bg_affine[1].ref_x = (bg_affine[1].ref_x & 0xFFFF) | hi;
        bg_affine[1].internal_x = bg_affine[1].ref_x;
        break;
    }
    case 0x3C:
        bg_affine[1].ref_y = (bg_affine[1].ref_y & ~0xFFFF) | static_cast<std::int32_t>(val);
        bg_affine[1].internal_y = bg_affine[1].ref_y;
        break;
    case 0x3E: {
        const std::int32_t hi = SignExtend28((static_cast<std::int32_t>(val) & 0x0FFF) << 16);
        bg_affine[1].ref_y = (bg_affine[1].ref_y & 0xFFFF) | hi;
        bg_affine[1].internal_y = bg_affine[1].ref_y;
        break;
    }
    case 0x40:
        winh[0] = val;
        break;
    case 0x42:
        winh[1] = val;
        break;
    case 0x44:
        winv[0] = val;
        break;
    case 0x46:
        winv[1] = val;
        break;
    case 0x48:
        winin = val & 0x3F3Fu;
        break;
    case 0x4A:
        winout = val & 0x3F3Fu;
        break;
    case 0x4C:
        mosaic = val;
        break;
    case 0x50:
        bldcnt = val & 0x3FFFu;
        break;
    case 0x52:
        bldalpha = val & 0x1F1Fu;
        break;
    case 0x54:
        bldy = val & 0x001Fu;
        break;
    default:
        break;
    }
}

void Ppu::RenderScanline(std::size_t y) {
    const std::size_t row_start = y * ScreenWidth;
    if ((dispcnt & (1u << 7)) != 0) {
        std::fill_n(raw_frame.begin() + static_cast<std::ptrdiff_t>(row_start), ScreenWidth,
                    std::uint16_t{0x7FFFu});
        return;
    }

    const std::uint16_t mode = dispcnt & 0x7u;
    const std::uint16_t backdrop = PaletteU16(0);

    std::array<std::array<std::uint16_t, ScreenWidth>, 4> bg_lines{};
    std::array<std::array<bool, ScreenWidth>, 4> bg_opaque{};
    std::array<bool, 4> bg_enabled{};

    switch (mode) {
    case 0:
        for (std::size_t bg = 0; bg < 4; ++bg) {
            if ((dispcnt & (1u << (8u + bg))) != 0) {
                bg_enabled[bg] = true;
                RenderTextBg(bg, y, bg_lines[bg], bg_opaque[bg]);
            }
        }
        break;
    case 1:
        for (std::size_t bg = 0; bg < 2; ++bg) {
            if ((dispcnt & (1u << (8u + bg))) != 0) {
                bg_enabled[bg] = true;
                RenderTextBg(bg, y, bg_lines[bg], bg_opaque[bg]);
            }
        }
        if ((dispcnt & (1u << 10)) != 0) {
            bg_enabled[2] = true;
            RenderAffineBg(2, bg_lines[2], bg_opaque[2]);
        }
        break;
    case 2:
        for (std::size_t bg = 2; bg < 4; ++bg) {
            if ((dispcnt & (1u << (8u + bg))) != 0) {
                bg_enabled[bg] = true;
                RenderAffineBg(bg, bg_lines[bg], bg_opaque[bg]);
            }
        }
        break;
    case 3:
        if ((dispcnt & (1u << 10)) != 0) {
            bg_enabled[2] = true;
            for (std::size_t x = 0; x < ScreenWidth; ++x) {
                bg_lines[2][x] = VramU16((y * ScreenWidth + x) * 2u) & 0x7FFFu;
                bg_opaque[2][x] = true;
            }
        }
        break;
    case 4:
        if ((dispcnt & (1u << 10)) != 0) {
            bg_enabled[2] = true;
            const std::size_t page_base = ((dispcnt & (1u << 4)) != 0) ? 0xA000u : 0u;
            for (std::size_t x = 0; x < ScreenWidth; ++x) {
                const std::size_t idx = vram[page_base + y * ScreenWidth + x];
                if (idx != 0) {
                    bg_lines[2][x] = PaletteU16(idx);
                    bg_opaque[2][x] = true;
                }
            }
        }
        break;
    case 5:
        if ((dispcnt & (1u << 10)) != 0) {
            bg_enabled[2] = true;
            const std::size_t page_base = ((dispcnt & (1u << 4)) != 0) ? 0xA000u : 0u;
            if (y < 128) {
                for (std::size_t x = 0; x < 160; ++x) {
                    bg_lines[2][x] = VramU16(page_base + (y * 160u + x) * 2u) & 0x7FFFu;
                    bg_opaque[2][x] = true;
                }
            }
        }
        break;
    default:
        break;
    }

    std::array<std::uint16_t, ScreenWidth> obj_color{};
    std::array<std::uint8_t, ScreenWidth> obj_prio{};
    obj_prio.fill(4);
    std::array<bool, ScreenWidth> obj_semi{};
    std::array<bool, ScreenWidth> obj_win_mask{};
    if ((dispcnt & (1u << 12)) != 0) {
        RenderSprites(y, obj_color, obj_prio, obj_semi, obj_win_mask);
    }

    const bool win_active = (dispcnt & 0xE000u) != 0;
    const bool win0_y_in = win_active && ((dispcnt & (1u << 13)) != 0) && InWinV(winv[0], y);
    const bool win1_y_in = win_active && ((dispcnt & (1u << 14)) != 0) && InWinV(winv[1], y);
    const bool objwin_en = win_active && ((dispcnt & (1u << 15)) != 0);

    const std::uint16_t bld_mode = static_cast<std::uint16_t>((bldcnt >> 6) & 0x3u);
    const std::uint16_t eva = (std::min<std::uint16_t>)(bldalpha & 0x1Fu, 16u);
    const std::uint16_t evb = (std::min<std::uint16_t>)((bldalpha >> 8) & 0x1Fu, 16u);
    const std::uint16_t evy = (std::min<std::uint16_t>)(bldy & 0x1Fu, 16u);

    auto layer_bit = [](std::size_t layer) -> std::uint16_t {
        if (layer == 0) {
            return 1u << 4;
        }
        if (layer <= 4) {
            return static_cast<std::uint16_t>(1u << (layer - 1u));
        }
        return 1u << 5;
    };

    for (std::size_t x = 0; x < ScreenWidth; ++x) {
        std::uint8_t win_flags = 0x3Fu;
        if (win_active) {
            if (win0_y_in && InWinH(winh[0], x)) {
                win_flags = static_cast<std::uint8_t>(winin & 0x3Fu);
            } else if (win1_y_in && InWinH(winh[1], x)) {
                win_flags = static_cast<std::uint8_t>((winin >> 8) & 0x3Fu);
            } else if (objwin_en && obj_win_mask[x]) {
                win_flags = static_cast<std::uint8_t>((winout >> 8) & 0x3Fu);
            } else {
                win_flags = static_cast<std::uint8_t>(winout & 0x3Fu);
            }
        }

        std::uint16_t top_color = backdrop;
        std::size_t top_layer = 5;
        std::uint8_t top_prio = 4;
        std::uint16_t sec_color = backdrop;
        std::size_t sec_layer = 5;
        std::uint8_t sec_prio = 4;

        auto consider = [&](std::size_t layer, std::uint8_t prio, std::uint16_t color) {
            if (prio < top_prio || (prio == top_prio && layer < top_layer)) {
                sec_color = top_color;
                sec_layer = top_layer;
                sec_prio = top_prio;
                top_color = color;
                top_layer = layer;
                top_prio = prio;
            } else if (prio < sec_prio || (prio == sec_prio && layer < sec_layer)) {
                sec_color = color;
                sec_layer = layer;
                sec_prio = prio;
            }
        };

        if ((win_flags & (1u << 4)) != 0 && obj_prio[x] < 4) {
            consider(0, obj_prio[x], obj_color[x]);
        }
        for (std::size_t bg = 0; bg < 4; ++bg) {
            if (bg_enabled[bg] && ((win_flags & (1u << bg)) != 0) && bg_opaque[bg][x]) {
                const auto prio = static_cast<std::uint8_t>(bgcnt[bg] & 3u);
                consider(bg + 1u, prio, bg_lines[bg][x]);
            }
        }

        const bool first_target = (bldcnt & layer_bit(top_layer)) != 0;
        const bool second_target = ((bldcnt >> 8) & layer_bit(sec_layer)) != 0;

        std::uint16_t final_color = top_color;
        if (top_layer == 0 && obj_semi[x] && second_target) {
            final_color = AlphaBlend(top_color, sec_color, eva, evb);
        } else if ((win_flags & (1u << 5)) != 0) {
            if (bld_mode == 1 && first_target && second_target) {
                final_color = AlphaBlend(top_color, sec_color, eva, evb);
            } else if (bld_mode == 2 && first_target) {
                final_color = Brighten(top_color, evy);
            } else if (bld_mode == 3 && first_target) {
                final_color = Darken(top_color, evy);
            }
        }

        raw_frame[row_start + x] = final_color;
    }
}

void Ppu::RenderTextBg(std::size_t bg, std::size_t y,
                       std::array<std::uint16_t, ScreenWidth>& line,
                       std::array<bool, ScreenWidth>& opaque) const {
    const std::uint16_t cnt = bgcnt[bg];
    const std::size_t char_base = static_cast<std::size_t>((cnt >> 2) & 0x3u) * 0x4000u;
    const bool is_8bpp = (cnt & (1u << 7)) != 0;
    const std::size_t screen_base = static_cast<std::size_t>((cnt >> 8) & 0x1Fu) * 0x800u;
    const std::uint16_t size = static_cast<std::uint16_t>((cnt >> 14) & 0x3u);
    const std::size_t width_px = ((size & 1u) != 0) ? 512u : 256u;
    const std::size_t height_px = ((size & 2u) != 0) ? 512u : 256u;

    const std::size_t py = (y + bgvofs[bg]) & (height_px - 1u);
    const std::size_t tile_y = py / 8u;
    const std::size_t fine_y = py % 8u;

    for (std::size_t x = 0; x < ScreenWidth; ++x) {
        const std::size_t px = (x + bghofs[bg]) & (width_px - 1u);
        const std::size_t tile_x = px / 8u;
        const std::size_t fine_x = px % 8u;

        std::size_t block = 0;
        if (tile_x >= 32u) {
            block += 1u;
        }
        if (tile_y >= 32u) {
            block += (width_px == 512u) ? 2u : 1u;
        }
        const std::size_t map_off =
            (screen_base + block * 0x800u + ((tile_y & 31u) * 32u + (tile_x & 31u)) * 2u) & 0xFFFFu;
        const std::uint16_t entry = VramU16(map_off);
        const std::size_t tile_num = entry & 0x3FFu;
        const bool hflip = (entry & (1u << 10)) != 0;
        const bool vflip = (entry & (1u << 11)) != 0;
        const std::size_t pal_bank = (entry >> 12) & 0xFu;

        const std::size_t tx = hflip ? (7u - fine_x) : fine_x;
        const std::size_t ty = vflip ? (7u - fine_y) : fine_y;

        if (is_8bpp) {
            const std::size_t addr = char_base + tile_num * 64u + ty * 8u + tx;
            if (addr < 0x10000u) {
                const std::size_t idx = vram[addr];
                if (idx != 0) {
                    line[x] = PaletteU16(idx);
                    opaque[x] = true;
                }
            }
        } else {
            const std::size_t addr = char_base + tile_num * 32u + ty * 4u + (tx / 2u);
            if (addr < 0x10000u) {
                const std::uint8_t byte = vram[addr];
                const std::size_t idx = ((tx & 1u) == 0) ? (byte & 0xFu) : (byte >> 4);
                if (idx != 0) {
                    line[x] = PaletteU16(pal_bank * 16u + idx);
                    opaque[x] = true;
                }
            }
        }
    }
}

void Ppu::RenderAffineBg(std::size_t bg, std::array<std::uint16_t, ScreenWidth>& line,
                         std::array<bool, ScreenWidth>& opaque) const {
    const std::uint16_t cnt = bgcnt[bg];
    const std::size_t char_base = static_cast<std::size_t>((cnt >> 2) & 0x3u) * 0x4000u;
    const std::size_t screen_base = static_cast<std::size_t>((cnt >> 8) & 0x1Fu) * 0x800u;
    const bool wrap = (cnt & (1u << 13)) != 0;
    const std::size_t size_tiles = std::size_t{16} << ((cnt >> 14) & 0x3u);
    const std::int32_t size_px = static_cast<std::int32_t>(size_tiles * 8u);

    const auto& aff = bg_affine[bg - 2u];
    std::int32_t cx = aff.internal_x;
    std::int32_t cy = aff.internal_y;

    for (std::size_t x = 0; x < ScreenWidth; ++x) {
        std::int32_t tx = cx >> 8;
        std::int32_t ty = cy >> 8;
        cx += static_cast<std::int32_t>(aff.pa);
        cy += static_cast<std::int32_t>(aff.pc);

        if (wrap) {
            tx = FloorMod(tx, size_px);
            ty = FloorMod(ty, size_px);
        } else if (tx < 0 || ty < 0 || tx >= size_px || ty >= size_px) {
            continue;
        }

        const std::size_t tile_x = static_cast<std::size_t>(tx) / 8u;
        const std::size_t tile_y = static_cast<std::size_t>(ty) / 8u;
        const std::size_t fine_x = static_cast<std::size_t>(tx) % 8u;
        const std::size_t fine_y = static_cast<std::size_t>(ty) % 8u;

        const std::size_t tile_num = vram[(screen_base + tile_y * size_tiles + tile_x) & 0xFFFFu];
        const std::size_t addr = char_base + tile_num * 64u + fine_y * 8u + fine_x;
        if (addr < 0x10000u) {
            const std::size_t idx = vram[addr];
            if (idx != 0) {
                line[x] = PaletteU16(idx);
                opaque[x] = true;
            }
        }
    }
}

void Ppu::RenderSprites(std::size_t y, std::array<std::uint16_t, ScreenWidth>& obj_color,
                        std::array<std::uint8_t, ScreenWidth>& obj_prio,
                        std::array<bool, ScreenWidth>& obj_semi,
                        std::array<bool, ScreenWidth>& obj_win_mask) const {
    struct SizePair {
        std::size_t w;
        std::size_t h;
    };
    static constexpr std::array<std::array<SizePair, 4>, 4> ObjSizes = {{
        {{{8, 8}, {16, 16}, {32, 32}, {64, 64}}},
        {{{16, 8}, {32, 8}, {32, 16}, {64, 32}}},
        {{{8, 16}, {8, 32}, {16, 32}, {32, 64}}},
        {{{8, 8}, {8, 8}, {8, 8}, {8, 8}}},
    }};

    const bool mapping_1d = (dispcnt & (1u << 6)) != 0;

    for (std::size_t idx = 0; idx < 128; ++idx) {
        const std::size_t base = idx * 8u;
        const std::uint16_t attr0 = static_cast<std::uint16_t>(oam[base]) |
                                    (static_cast<std::uint16_t>(oam[base + 1]) << 8);
        const std::uint16_t attr1 = static_cast<std::uint16_t>(oam[base + 2]) |
                                    (static_cast<std::uint16_t>(oam[base + 3]) << 8);
        const std::uint16_t attr2 = static_cast<std::uint16_t>(oam[base + 4]) |
                                    (static_cast<std::uint16_t>(oam[base + 5]) << 8);

        const std::uint16_t obj_mode = static_cast<std::uint16_t>((attr0 >> 8) & 0x3u);
        if (obj_mode == 2u) {
            continue;
        }
        const bool is_affine = (obj_mode & 1u) != 0;
        const bool double_size = obj_mode == 3u;
        const std::uint16_t gfx_mode = static_cast<std::uint16_t>((attr0 >> 10) & 0x3u);
        const bool is_8bpp = (attr0 & (1u << 13)) != 0;
        const std::size_t shape = (attr0 >> 14) & 0x3u;
        const std::size_t size_idx = (attr1 >> 14) & 0x3u;
        const auto [w, h] = ObjSizes[shape][size_idx];

        const std::size_t bound_w = double_size ? w * 2u : w;
        const std::size_t bound_h = double_size ? h * 2u : h;

        std::int32_t sy = static_cast<std::int32_t>(attr0 & 0xFFu);
        if (sy >= 160) {
            sy -= 256;
        }
        std::int32_t sx = static_cast<std::int32_t>(attr1 & 0x1FFu);
        if (sx >= 240) {
            sx -= 512;
        }

        const std::int32_t ly = static_cast<std::int32_t>(y) - sy;
        if (ly < 0 || ly >= static_cast<std::int32_t>(bound_h)) {
            continue;
        }

        const auto prio = static_cast<std::uint8_t>((attr2 >> 10) & 0x3u);
        const std::size_t tile_base = attr2 & 0x3FFu;
        const std::size_t pal_bank = (attr2 >> 12) & 0xFu;

        std::int32_t pa = 256;
        std::int32_t pb = 0;
        std::int32_t pc = 0;
        std::int32_t pd = 256;
        if (is_affine) {
            const std::size_t aff_idx = static_cast<std::size_t>((attr1 >> 9) & 0x1Fu) * 32u;
            const auto read_param = [&](std::size_t off) -> std::int32_t {
                const auto u = static_cast<std::uint16_t>(oam[off]) |
                               (static_cast<std::uint16_t>(oam[off + 1]) << 8);
                return static_cast<std::int32_t>(static_cast<std::int16_t>(u));
            };
            pa = read_param(aff_idx + 6u);
            pb = read_param(aff_idx + 14u);
            pc = read_param(aff_idx + 22u);
            pd = read_param(aff_idx + 30u);
        }

        const bool hflip = !is_affine && ((attr1 & (1u << 12)) != 0);
        const bool vflip = !is_affine && ((attr1 & (1u << 13)) != 0);

        const std::int32_t half_bw = static_cast<std::int32_t>(bound_w) / 2;
        const std::int32_t half_bh = static_cast<std::int32_t>(bound_h) / 2;
        const std::int32_t half_w = static_cast<std::int32_t>(w) / 2;
        const std::int32_t half_h = static_cast<std::int32_t>(h) / 2;

        for (std::int32_t local_x = 0; local_x < static_cast<std::int32_t>(bound_w); ++local_x) {
            const std::int32_t px = sx + local_x;
            if (px < 0 || px >= static_cast<std::int32_t>(ScreenWidth)) {
                continue;
            }
            const std::size_t ux = static_cast<std::size_t>(px);

            std::size_t tex_x = 0;
            std::size_t tex_y = 0;
            if (is_affine) {
                const std::int32_t dx = local_x - half_bw;
                const std::int32_t dy = ly - half_bh;
                const std::int32_t tx = ((pa * dx + pb * dy) >> 8) + half_w;
                const std::int32_t ty = ((pc * dx + pd * dy) >> 8) + half_h;
                if (tx < 0 || ty < 0 || tx >= static_cast<std::int32_t>(w) ||
                    ty >= static_cast<std::int32_t>(h)) {
                    continue;
                }
                tex_x = static_cast<std::size_t>(tx);
                tex_y = static_cast<std::size_t>(ty);
            } else {
                tex_x = hflip ? (w - 1u - static_cast<std::size_t>(local_x))
                              : static_cast<std::size_t>(local_x);
                tex_y = vflip ? (h - 1u - static_cast<std::size_t>(ly))
                              : static_cast<std::size_t>(ly);
            }

            const std::size_t tile_x = tex_x / 8u;
            const std::size_t tile_y = tex_y / 8u;
            const std::size_t fine_x = tex_x % 8u;
            const std::size_t fine_y = tex_y % 8u;

            std::size_t color_idx = 0;
            if (is_8bpp) {
                const std::size_t tile_stride = mapping_1d ? ((w / 8u) * 2u) : 32u;
                const std::size_t tile_num = (tile_base + tile_y * tile_stride + tile_x * 2u) & 0x3FFu;
                const std::size_t addr = 0x10000u + tile_num * 32u + fine_y * 8u + fine_x;
                if (addr < vram.size()) {
                    color_idx = vram[addr];
                }
            } else {
                const std::size_t tile_stride = mapping_1d ? (w / 8u) : 32u;
                const std::size_t tile_num = (tile_base + tile_y * tile_stride + tile_x) & 0x3FFu;
                const std::size_t addr = 0x10000u + tile_num * 32u + fine_y * 4u + (fine_x / 2u);
                if (addr < vram.size()) {
                    const std::uint8_t byte = vram[addr];
                    color_idx = ((fine_x & 1u) == 0) ? (byte & 0xFu) : (byte >> 4);
                }
            }

            if (color_idx == 0) {
                continue;
            }
            if (gfx_mode == 2u) {
                obj_win_mask[ux] = true;
                continue;
            }
            if (prio < obj_prio[ux]) {
                obj_prio[ux] = prio;
                const std::size_t pal_idx = is_8bpp ? (256u + color_idx) : (256u + pal_bank * 16u + color_idx);
                obj_color[ux] = PaletteU16(pal_idx);
                obj_semi[ux] = gfx_mode == 1u;
            }
        }
    }
}

} // namespace Emulation::Gba
