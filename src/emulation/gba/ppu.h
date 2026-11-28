// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "emulation/gba/types.h"

namespace Emulation::Gba {

struct BgAffine {
    std::int16_t pa = 0x100;
    std::int16_t pb = 0;
    std::int16_t pc = 0;
    std::int16_t pd = 0x100;
    std::int32_t ref_x = 0;
    std::int32_t ref_y = 0;
    std::int32_t internal_x = 0;
    std::int32_t internal_y = 0;
};

struct PpuStepEvents {
    std::uint16_t irq_mask = 0;
    bool entered_hblank = false;
    bool entered_vblank = false;
    bool frame_complete = false;
};

class Ppu {
public:
    std::vector<std::uint8_t> palette_ram;
    std::vector<std::uint8_t> vram;
    std::vector<std::uint8_t> oam;
    std::uint16_t dispcnt = 0x0080;
    std::uint16_t dispstat = 0;
    std::uint16_t vcount = 0;
    std::array<std::uint16_t, 4> bgcnt{};
    std::array<std::uint16_t, 4> bghofs{};
    std::array<std::uint16_t, 4> bgvofs{};
    std::array<BgAffine, 2> bg_affine{};
    std::array<std::uint16_t, 2> winh{};
    std::array<std::uint16_t, 2> winv{};
    std::uint16_t winin = 0;
    std::uint16_t winout = 0;
    std::uint16_t mosaic = 0;
    std::uint16_t bldcnt = 0;
    std::uint16_t bldalpha = 0;
    std::uint16_t bldy = 0;
    std::uint32_t cycle_in_line = 0;
    bool in_hblank = false;
    VideoOptions video_options{};
    std::vector<std::uint16_t> raw_frame;
    std::vector<std::uint16_t> prev_raw_frame;
    std::vector<std::uint32_t> output_frame;

    Ppu();
    void Reset();
    void RefreshOutputFrame();
    [[nodiscard]] PpuStepEvents Step(std::uint32_t cycles);

    [[nodiscard]] std::uint16_t ReadIoU16(std::uint32_t offset) const;
    void WriteIoU16(std::uint32_t offset, std::uint16_t val);

private:
    [[nodiscard]] std::uint16_t PaletteU16(std::size_t index) const {
        const std::size_t off = (index * 2u) & 0x3FEu;
        return (static_cast<std::uint16_t>(palette_ram[off]) |
                (static_cast<std::uint16_t>(palette_ram[off + 1]) << 8)) &
               0x7FFFu;
    }

    [[nodiscard]] std::uint16_t VramU16(std::size_t offset) const {
        const std::size_t off = offset & ~std::size_t{1};
        if (off + 1 < vram.size()) {
            return static_cast<std::uint16_t>(vram[off]) |
                   (static_cast<std::uint16_t>(vram[off + 1]) << 8);
        }
        return 0;
    }

    void RenderScanline(std::size_t y);
    void RenderTextBg(std::size_t bg, std::size_t y, std::array<std::uint16_t, ScreenWidth>& line,
                      std::array<bool, ScreenWidth>& opaque) const;
    void RenderAffineBg(std::size_t bg, std::array<std::uint16_t, ScreenWidth>& line,
                        std::array<bool, ScreenWidth>& opaque) const;
    void RenderSprites(std::size_t y, std::array<std::uint16_t, ScreenWidth>& obj_color,
                       std::array<std::uint8_t, ScreenWidth>& obj_prio,
                       std::array<bool, ScreenWidth>& obj_semi,
                       std::array<bool, ScreenWidth>& obj_win_mask) const;
};

[[nodiscard]] std::uint32_t Rgb555ToAbgr8888(std::uint16_t rgb555, bool color_correction);

} // namespace Emulation::Gba
