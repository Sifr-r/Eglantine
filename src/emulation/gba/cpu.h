// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <bit>
#include <cstdint>

#include "emulation/gba/bus.h"
#include "emulation/gba/cpu_regs.h"

namespace Emulation::Gba {

class Cpu {
public:
    CpuRegs regs;

    explicit Cpu(bool with_bios = false);
    void Reset(bool with_bios);
    [[nodiscard]] std::uint32_t Step(Bus& bus);

private:
    void TriggerIrq(Bus& bus);
};

[[nodiscard]] std::uint32_t ExecArm(std::uint32_t op, CpuRegs& regs, Bus& bus);
[[nodiscard]] std::uint32_t ExecThumb(std::uint16_t op, CpuRegs& regs, Bus& bus);

[[nodiscard]] inline std::uint32_t AddWithFlags(CpuRegs& regs, std::uint32_t a, std::uint32_t b,
                                                bool update_flags) {
    const std::uint64_t sum = static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b);
    const std::uint32_t res = static_cast<std::uint32_t>(sum);
    if (update_flags) {
        regs.SetNZ(res);
        regs.SetFlag(FlagC, sum > 0xFFFFFFFFull);
        const bool overflow = (((a ^ b) & 0x80000000u) == 0) && (((a ^ res) & 0x80000000u) != 0);
        regs.SetFlag(FlagV, overflow);
    }
    return res;
}

[[nodiscard]] inline std::uint32_t AdcWithFlags(CpuRegs& regs, std::uint32_t a, std::uint32_t b,
                                                bool update_flags) {
    const std::uint64_t c_in = ((regs.cpsr & FlagC) != 0) ? 1ull : 0ull;
    const std::uint64_t sum = static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b) + c_in;
    const std::uint32_t res = static_cast<std::uint32_t>(sum);
    if (update_flags) {
        regs.SetNZ(res);
        regs.SetFlag(FlagC, sum > 0xFFFFFFFFull);
        const bool overflow = (((a ^ b) & 0x80000000u) == 0) && (((a ^ res) & 0x80000000u) != 0);
        regs.SetFlag(FlagV, overflow);
    }
    return res;
}

[[nodiscard]] inline std::uint32_t SubWithFlags(CpuRegs& regs, std::uint32_t a, std::uint32_t b,
                                                bool update_flags) {
    const std::uint32_t res = a - b;
    if (update_flags) {
        regs.SetNZ(res);
        regs.SetFlag(FlagC, a >= b);
        const bool overflow = (((a ^ b) & 0x80000000u) != 0) && (((a ^ res) & 0x80000000u) != 0);
        regs.SetFlag(FlagV, overflow);
    }
    return res;
}

[[nodiscard]] inline std::uint32_t SbcWithFlags(CpuRegs& regs, std::uint32_t a, std::uint32_t b,
                                                bool update_flags) {
    const std::int64_t borrow_in = ((regs.cpsr & FlagC) != 0) ? 0 : 1;
    const std::int64_t diff =
        static_cast<std::int64_t>(a) - static_cast<std::int64_t>(b) - borrow_in;
    const std::uint32_t res = static_cast<std::uint32_t>(diff);
    if (update_flags) {
        regs.SetNZ(res);
        regs.SetFlag(FlagC, diff >= 0);
        const bool overflow = (((a ^ b) & 0x80000000u) != 0) && (((a ^ res) & 0x80000000u) != 0);
        regs.SetFlag(FlagV, overflow);
    }
    return res;
}

[[nodiscard]] inline std::uint32_t CpuReadU32Rotated(Bus& bus, std::uint32_t addr) {
    const std::uint32_t word = bus.ReadU32(addr & ~3u);
    const int rot = static_cast<int>((addr & 3u) * 8u);
    return std::rotr(word, rot);
}

[[nodiscard]] inline std::uint32_t CpuReadU16Rotated(Bus& bus, std::uint32_t addr) {
    const std::uint32_t half = bus.ReadU16(addr & ~1u);
    return ((addr & 1u) != 0) ? std::rotr(half, 8) : half;
}

[[nodiscard]] inline std::uint32_t CpuReadS16Arm(Bus& bus, std::uint32_t addr) {
    if ((addr & 1u) != 0) {
        return static_cast<std::uint32_t>(
            static_cast<std::int32_t>(static_cast<std::int8_t>(bus.ReadU8(addr))));
    }
    return static_cast<std::uint32_t>(
        static_cast<std::int32_t>(static_cast<std::int16_t>(bus.ReadU16(addr))));
}

inline void UpdateNZC(CpuRegs& regs, std::uint32_t val, bool carry) {
    regs.SetNZ(val);
    regs.SetFlag(FlagC, carry);
}

} // namespace Emulation::Gba
