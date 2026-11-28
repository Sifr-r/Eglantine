// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "emulation/gba/types.h"

namespace Emulation::Gba {

enum class DmaTrigger : std::uint8_t {
    Immediate = 0,
    VBlank = 1,
    HBlank = 2,
    Special = 3,
};

struct DmaChannel {
    std::uint32_t sad = 0;
    std::uint32_t dad = 0;
    std::uint16_t count = 0;
    std::uint16_t control = 0;
    std::uint32_t internal_sad = 0;
    std::uint32_t internal_dad = 0;
    std::uint32_t internal_count = 0;

    [[nodiscard]] bool Enabled() const {
        return (control & (1u << 15)) != 0;
    }

    [[nodiscard]] bool IrqEnabled() const {
        return (control & (1u << 14)) != 0;
    }

    [[nodiscard]] DmaTrigger Timing() const {
        return static_cast<DmaTrigger>((control >> 12) & 0x3u);
    }

    [[nodiscard]] bool Is32Bit() const {
        return (control & (1u << 10)) != 0;
    }

    [[nodiscard]] bool Repeat() const {
        return (control & (1u << 9)) != 0;
    }

    [[nodiscard]] std::int32_t SrcAdj() const {
        switch ((control >> 7) & 0x3u) {
        case 1:
            return -1;
        case 2:
            return 0;
        default:
            return 1;
        }
    }

    [[nodiscard]] std::int32_t DstAdj() const {
        switch ((control >> 5) & 0x3u) {
        case 1:
            return -1;
        case 2:
            return 0;
        default:
            return 1;
        }
    }

    [[nodiscard]] bool DstReload() const {
        return ((control >> 5) & 0x3u) == 3u;
    }
};

struct DmaController {
    std::array<DmaChannel, 4> channels{};

    void Reset();
    [[nodiscard]] static std::uint16_t IrqBit(std::size_t idx);
    [[nodiscard]] bool WriteControl(std::size_t idx, std::uint16_t val);
};

} // namespace Emulation::Gba
