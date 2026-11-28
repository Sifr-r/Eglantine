// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "emulation/gba/dma.h"

namespace Emulation::Gba {

void DmaController::Reset() {
    channels = {};
}

std::uint16_t DmaController::IrqBit(std::size_t idx) {
    switch (idx) {
    case 0:
        return IrqDma0;
    case 1:
        return IrqDma1;
    case 2:
        return IrqDma2;
    default:
        return IrqDma3;
    }
}

bool DmaController::WriteControl(std::size_t idx, std::uint16_t val) {
    auto& ch = channels[idx];
    const bool was_enabled = ch.Enabled();
    ch.control = val;
    if (!was_enabled && ch.Enabled()) {
        ch.internal_sad = ch.sad;
        ch.internal_dad = ch.dad;
        const std::uint32_t raw_cnt = ch.count & (idx == 3 ? 0xFFFFu : 0x3FFFu);
        ch.internal_count = (raw_cnt == 0) ? (idx == 3 ? 0x10000u : 0x4000u) : raw_cnt;
        return ch.Timing() == DmaTrigger::Immediate;
    }
    return false;
}

} // namespace Emulation::Gba
