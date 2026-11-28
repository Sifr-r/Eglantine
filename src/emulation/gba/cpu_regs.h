// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Emulation::Gba {

enum class CpuMode : std::uint8_t {
    User = 0x10,
    Fiq = 0x11,
    Irq = 0x12,
    Svc = 0x13,
    Abort = 0x17,
    Undefined = 0x1B,
    System = 0x1F,
};

[[nodiscard]] inline std::optional<CpuMode> CpuModeFromBits(std::uint32_t bits) {
    switch (bits & 0x1Fu) {
    case 0x10:
        return CpuMode::User;
    case 0x11:
        return CpuMode::Fiq;
    case 0x12:
        return CpuMode::Irq;
    case 0x13:
        return CpuMode::Svc;
    case 0x17:
        return CpuMode::Abort;
    case 0x1B:
        return CpuMode::Undefined;
    case 0x1F:
        return CpuMode::System;
    default:
        return std::nullopt;
    }
}

inline constexpr std::uint32_t FlagN = 1u << 31;
inline constexpr std::uint32_t FlagZ = 1u << 30;
inline constexpr std::uint32_t FlagC = 1u << 29;
inline constexpr std::uint32_t FlagV = 1u << 28;
inline constexpr std::uint32_t FlagI = 1u << 7;
inline constexpr std::uint32_t FlagF = 1u << 6;
inline constexpr std::uint32_t FlagT = 1u << 5;

struct CpuRegs {
    std::array<std::uint32_t, 16> r{};
    std::uint32_t cpsr = 0x1F;
    std::array<std::uint32_t, 7> r_fiq{};    // R8..R14
    std::array<std::uint32_t, 5> r_usr_hi{}; // R8..R12 non-FIQ
    std::uint32_t sp_usr = 0x03007F00;
    std::uint32_t lr_usr = 0;
    std::uint32_t sp_irq = 0x03007FA0;
    std::uint32_t lr_irq = 0;
    std::uint32_t sp_svc = 0x03007FE0;
    std::uint32_t lr_svc = 0;
    std::uint32_t sp_abt = 0x03007F00;
    std::uint32_t lr_abt = 0;
    std::uint32_t sp_und = 0x03007F00;
    std::uint32_t lr_und = 0;
    std::uint32_t spsr_fiq = 0;
    std::uint32_t spsr_irq = 0;
    std::uint32_t spsr_svc = 0;
    std::uint32_t spsr_abt = 0;
    std::uint32_t spsr_und = 0;
    bool halted = false;

    explicit CpuRegs(bool with_bios = false);
    void Reset(bool with_bios);

    [[nodiscard]] CpuMode Mode() const {
        return CpuModeFromBits(cpsr).value_or(CpuMode::System);
    }

    [[nodiscard]] bool IsThumb() const {
        return (cpsr & FlagT) != 0;
    }

    [[nodiscard]] bool IrqDisabled() const {
        return (cpsr & FlagI) != 0;
    }

    void SetNZ(std::uint32_t val) {
        cpsr &= ~(FlagN | FlagZ);
        if ((val & 0x80000000u) != 0) {
            cpsr |= FlagN;
        }
        if (val == 0) {
            cpsr |= FlagZ;
        }
    }

    void SetFlag(std::uint32_t flag, bool on) {
        if (on) {
            cpsr |= flag;
        } else {
            cpsr &= ~flag;
        }
    }

    [[nodiscard]] bool ConditionPassed(std::uint32_t cond) const {
        // Branchless 16-bit mask per condition code indexed by NZCV (top 4 bits of CPSR).
        static constexpr std::array<std::uint16_t, 16> CondMask = {
            0xF0F0, // 0: EQ (Z == 1)
            0x0F0F, // 1: NE (Z == 0)
            0xCCCC, // 2: CS (C == 1)
            0x3333, // 3: CC (C == 0)
            0xFF00, // 4: MI (N == 1)
            0x00FF, // 5: PL (N == 0)
            0xAAAA, // 6: VS (V == 1)
            0x5555, // 7: VC (V == 0)
            0x0C0C, // 8: HI (C == 1 && Z == 0)
            0xF3F3, // 9: LS (C == 0 || Z == 1)
            0xAA55, // A: GE (N == V)
            0x55AA, // B: LT (N != V)
            0x0A05, // C: GT (Z == 0 && N == V)
            0xF5FA, // D: LE (Z == 1 || N != V)
            0xFFFF, // E: AL
            0x0000, // F: NV
        };
        return ((CondMask[cond & 0xFu] >> (cpsr >> 28)) & 1u) != 0;
    }

    [[nodiscard]] std::uint32_t Spsr() const;
    void SetSpsr(std::uint32_t val);
    void SwitchMode(CpuMode new_mode);
    void SetCpsr(std::uint32_t val);

    [[nodiscard]] std::uint32_t ReadUserReg(std::size_t reg) const;
    void WriteUserReg(std::size_t reg, std::uint32_t val);
};

} // namespace Emulation::Gba
