// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Emulation::Gba {

struct CpuRegs;
class Bus;

inline constexpr std::uint32_t HleIrqReturnAddr = 0x00000138u;
inline constexpr std::uint32_t HleWaitLoopAddr = 0x00000140u;

class Bios {
public:
    std::uint32_t latch = 0xE129F000u;
    bool intr_wait_active = false;
    std::uint16_t intr_wait_mask = 0;

    [[nodiscard]] static bool Create(std::span<const std::uint8_t> bytes, Bios& out,
                                     std::string& error);

    [[nodiscard]] bool HasRealBios() const {
        return !data_.empty();
    }

    [[nodiscard]] std::uint8_t ReadU8(std::uint32_t addr, std::uint32_t pc);
    [[nodiscard]] std::uint16_t ReadU16(std::uint32_t addr, std::uint32_t pc);
    [[nodiscard]] std::uint32_t ReadU32(std::uint32_t addr, std::uint32_t pc);

private:
    std::vector<std::uint8_t> data_;
};

void EnterHleIrq(CpuRegs& regs, Bus& bus, std::uint32_t return_lr);
void FinishHleIrq(CpuRegs& regs, Bus& bus);
void HleSwi(std::uint8_t comment, CpuRegs& regs, Bus& bus);

} // namespace Emulation::Gba
