// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "emulation/gba/timers.h"
#include "emulation/gba/types.h"

namespace Emulation::Gba {

void Timers::Reset() {
    channels = {};
}

std::uint16_t Timers::ReadU16(std::size_t idx, bool is_control) const {
    return is_control ? channels[idx].control : channels[idx].counter;
}

void Timers::WriteReload(std::size_t idx, std::uint16_t val) {
    channels[idx].reload = val;
}

void Timers::WriteControl(std::size_t idx, std::uint16_t val) {
    const bool was_enabled = channels[idx].Enabled();
    const bool now_enabled = (val & (1u << 7)) != 0;
    channels[idx].control = val & 0x00C7u;
    if (!was_enabled && now_enabled) {
        channels[idx].counter = channels[idx].reload;
        channels[idx].subcycles = 0;
    }
}

TimerStepResult Timers::Tick(std::uint32_t cycles) {
    static constexpr std::array<std::uint16_t, 4> IrqBits = {
        IrqTimer0,
        IrqTimer1,
        IrqTimer2,
        IrqTimer3,
    };

    TimerStepResult result{};
    std::uint32_t prev_overflows = 0;

    for (std::size_t i = 0; i < channels.size(); ++i) {
        auto& ch = channels[i];
        if (!ch.Enabled()) {
            prev_overflows = 0;
            continue;
        }

        std::uint32_t ticks = 0;
        if (i > 0 && ch.Cascade()) {
            ticks = prev_overflows;
        } else {
            const std::uint32_t shift = ch.PrescalerShift();
            ch.subcycles += cycles;
            const std::uint32_t step = 1u << shift;
            if (ch.subcycles >= step) {
                ticks = ch.subcycles >> shift;
                ch.subcycles &= step - 1u;
            }
        }

        std::uint32_t overflows = 0;
        if (ticks > 0) {
            const std::uint32_t to_first = static_cast<std::uint32_t>(0xFFFFu - ch.counter) + 1u;
            if (ticks < to_first) {
                ch.counter = static_cast<std::uint16_t>(ch.counter + ticks);
            } else {
                const std::uint32_t period = static_cast<std::uint32_t>(0xFFFFu - ch.reload) + 1u;
                const std::uint32_t rem = ticks - to_first;
                overflows = 1u + (rem / period);
                ch.counter = static_cast<std::uint16_t>(ch.reload + (rem % period));
                result.overflow_mask |= static_cast<std::uint8_t>(1u << i);
                if (ch.IrqEnabled()) {
                    result.irq_mask |= IrqBits[i];
                }
            }
        }
        prev_overflows = overflows;
    }

    return result;
}

} // namespace Emulation::Gba
