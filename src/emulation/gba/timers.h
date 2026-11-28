// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Emulation::Gba {

struct TimerChannel {
    std::uint16_t counter = 0;
    std::uint16_t reload = 0;
    std::uint16_t control = 0;
    std::uint32_t subcycles = 0;

    [[nodiscard]] bool Enabled() const {
        return (control & (1u << 7)) != 0;
    }

    [[nodiscard]] bool Cascade() const {
        return (control & (1u << 2)) != 0;
    }

    [[nodiscard]] bool IrqEnabled() const {
        return (control & (1u << 6)) != 0;
    }

    [[nodiscard]] std::uint32_t PrescalerShift() const {
        static constexpr std::array<std::uint32_t, 4> PrescalerShifts = {0, 6, 8, 10};
        return PrescalerShifts[control & 0x3u];
    }
};

struct TimerStepResult {
    std::uint16_t irq_mask = 0;
    std::uint8_t overflow_mask = 0;
};

struct Timers {
    std::array<TimerChannel, 4> channels{};

    void Reset();
    [[nodiscard]] std::uint16_t ReadU16(std::size_t idx, bool is_control) const;
    void WriteReload(std::size_t idx, std::uint16_t val);
    void WriteControl(std::size_t idx, std::uint16_t val);
    [[nodiscard]] TimerStepResult Tick(std::uint32_t cycles);
};

} // namespace Emulation::Gba
