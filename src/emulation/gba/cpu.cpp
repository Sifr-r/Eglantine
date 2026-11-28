// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "emulation/gba/cpu.h"
#include "emulation/gba/bios.h"

namespace Emulation::Gba {

Cpu::Cpu(bool with_bios) : regs(with_bios) {}

void Cpu::Reset(bool with_bios) {
    regs.Reset(with_bios);
}

std::uint32_t Cpu::Step(Bus& bus) {
    if (bus.halt_requested || bus.stop_requested) {
        regs.halted = true;
        bus.halt_requested = false;
        bus.stop_requested = false;
    }

    if (bus.AnyEnabledIrqFlagged()) {
        regs.halted = false;
    }

    if (regs.halted) {
        return 16;
    }

    if (bus.IrqPending() && !regs.IrqDisabled()) {
        TriggerIrq(bus);
        return 3;
    }

    const std::uint32_t pc = regs.r[15];
    bus.cpu_pc = pc;

    if (!regs.IsThumb() && !bus.bios.HasRealBios() && pc == HleIrqReturnAddr) {
        FinishHleIrq(regs, bus);
        return 3;
    }

    if (regs.IsThumb()) {
        const std::uint32_t aligned_pc = pc & ~1u;
        const std::uint16_t op = bus.ReadU16(aligned_pc);
        bus.open_bus_latch = static_cast<std::uint32_t>(op) * 0x00010001u;
        regs.r[15] = aligned_pc + 2u;
        return ExecThumb(op, regs, bus);
    }

    const std::uint32_t aligned_pc = pc & ~3u;
    const std::uint32_t op = bus.ReadU32(aligned_pc);
    bus.open_bus_latch = op;
    regs.r[15] = aligned_pc + 4u;
    return ExecArm(op, regs, bus);
}

void Cpu::TriggerIrq(Bus& bus) {
    const std::uint32_t return_lr = regs.r[15] + 4u;
    if (bus.bios.HasRealBios()) {
        const std::uint32_t old_cpsr = regs.cpsr;
        regs.SwitchMode(CpuMode::Irq);
        regs.spsr_irq = old_cpsr;
        regs.cpsr = (regs.cpsr & ~FlagT) | FlagI;
        regs.r[14] = return_lr;
        regs.r[15] = 0x00000018u;
    } else {
        EnterHleIrq(regs, bus, return_lr);
    }
}

} // namespace Emulation::Gba
