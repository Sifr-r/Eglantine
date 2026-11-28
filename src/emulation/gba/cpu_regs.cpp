// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "emulation/gba/cpu_regs.h"

#include <algorithm>

namespace Emulation::Gba {

CpuRegs::CpuRegs(bool with_bios) {
    Reset(with_bios);
}

void CpuRegs::Reset(bool with_bios) {
    r.fill(0);
    cpsr = with_bios ? 0xD3u : 0x1Fu;
    r_fiq.fill(0);
    r_usr_hi.fill(0);
    sp_usr = 0x03007F00;
    lr_usr = 0;
    sp_irq = 0x03007FA0;
    lr_irq = 0;
    sp_svc = 0x03007FE0;
    lr_svc = 0;
    sp_abt = 0x03007F00;
    lr_abt = 0;
    sp_und = 0x03007F00;
    lr_und = 0;
    spsr_fiq = 0;
    spsr_irq = 0;
    spsr_svc = 0;
    spsr_abt = 0;
    spsr_und = 0;
    halted = false;

    if (with_bios) {
        r[15] = 0x00000000;
        r[13] = sp_svc;
    } else {
        r[15] = 0x08000000;
        r[13] = sp_usr;
    }
}

std::uint32_t CpuRegs::Spsr() const {
    switch (Mode()) {
    case CpuMode::Fiq:
        return spsr_fiq;
    case CpuMode::Irq:
        return spsr_irq;
    case CpuMode::Svc:
        return spsr_svc;
    case CpuMode::Abort:
        return spsr_abt;
    case CpuMode::Undefined:
        return spsr_und;
    case CpuMode::User:
    case CpuMode::System:
        return cpsr;
    }
    return cpsr;
}

void CpuRegs::SetSpsr(std::uint32_t val) {
    switch (Mode()) {
    case CpuMode::Fiq:
        spsr_fiq = val;
        break;
    case CpuMode::Irq:
        spsr_irq = val;
        break;
    case CpuMode::Svc:
        spsr_svc = val;
        break;
    case CpuMode::Abort:
        spsr_abt = val;
        break;
    case CpuMode::Undefined:
        spsr_und = val;
        break;
    case CpuMode::User:
    case CpuMode::System:
        break;
    }
}

void CpuRegs::SwitchMode(CpuMode new_mode) {
    const CpuMode old_mode = Mode();
    if (old_mode == new_mode) {
        return;
    }

    if (old_mode == CpuMode::Fiq) {
        std::copy_n(r.begin() + 8, 7, r_fiq.begin());
        std::copy_n(r_usr_hi.begin(), 5, r.begin() + 8);
    } else {
        if (new_mode == CpuMode::Fiq) {
            std::copy_n(r.begin() + 8, 5, r_usr_hi.begin());
        }
        switch (old_mode) {
        case CpuMode::User:
        case CpuMode::System:
            sp_usr = r[13];
            lr_usr = r[14];
            break;
        case CpuMode::Irq:
            sp_irq = r[13];
            lr_irq = r[14];
            break;
        case CpuMode::Svc:
            sp_svc = r[13];
            lr_svc = r[14];
            break;
        case CpuMode::Abort:
            sp_abt = r[13];
            lr_abt = r[14];
            break;
        case CpuMode::Undefined:
            sp_und = r[13];
            lr_und = r[14];
            break;
        case CpuMode::Fiq:
            break;
        }
    }

    if (new_mode == CpuMode::Fiq) {
        std::copy_n(r_fiq.begin(), 7, r.begin() + 8);
    } else {
        switch (new_mode) {
        case CpuMode::User:
        case CpuMode::System:
            r[13] = sp_usr;
            r[14] = lr_usr;
            break;
        case CpuMode::Irq:
            r[13] = sp_irq;
            r[14] = lr_irq;
            break;
        case CpuMode::Svc:
            r[13] = sp_svc;
            r[14] = lr_svc;
            break;
        case CpuMode::Abort:
            r[13] = sp_abt;
            r[14] = lr_abt;
            break;
        case CpuMode::Undefined:
            r[13] = sp_und;
            r[14] = lr_und;
            break;
        case CpuMode::Fiq:
            break;
        }
    }

    cpsr = (cpsr & ~0x1Fu) | static_cast<std::uint32_t>(new_mode);
}

void CpuRegs::SetCpsr(std::uint32_t val) {
    if (const auto new_mode = CpuModeFromBits(val)) {
        SwitchMode(*new_mode);
        cpsr = val;
    } else {
        cpsr = (val & ~0x1Fu) | (cpsr & 0x1Fu);
    }
}

std::uint32_t CpuRegs::ReadUserReg(std::size_t reg) const {
    if (reg <= 7 || reg == 15) {
        return r[reg];
    }
    if (reg <= 12) {
        return (Mode() == CpuMode::Fiq) ? r_usr_hi[reg - 8] : r[reg];
    }
    if (reg == 13) {
        const auto m = Mode();
        return (m == CpuMode::User || m == CpuMode::System) ? r[13] : sp_usr;
    }
    if (reg == 14) {
        const auto m = Mode();
        return (m == CpuMode::User || m == CpuMode::System) ? r[14] : lr_usr;
    }
    return 0;
}

void CpuRegs::WriteUserReg(std::size_t reg, std::uint32_t val) {
    if (reg <= 7 || reg == 15) {
        r[reg] = val;
        return;
    }
    if (reg <= 12) {
        if (Mode() == CpuMode::Fiq) {
            r_usr_hi[reg - 8] = val;
        } else {
            r[reg] = val;
        }
        return;
    }
    if (reg == 13) {
        const auto m = Mode();
        if (m == CpuMode::User || m == CpuMode::System) {
            r[13] = val;
        } else {
            sp_usr = val;
        }
        return;
    }
    if (reg == 14) {
        const auto m = Mode();
        if (m == CpuMode::User || m == CpuMode::System) {
            r[14] = val;
        } else {
            lr_usr = val;
        }
    }
}

} // namespace Emulation::Gba
