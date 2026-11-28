// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "emulation/gba/apu.h"
#include "emulation/gba/bios.h"
#include "emulation/gba/cartridge.h"
#include "emulation/gba/dma.h"
#include "emulation/gba/ppu.h"
#include "emulation/gba/timers.h"

namespace Emulation::Gba {

inline constexpr std::size_t EwramSize = 256 * 1024;
inline constexpr std::size_t IwramSize = 32 * 1024;

class Bus {
public:
    Bios bios{};
    Cartridge cartridge{};
    std::vector<std::uint8_t> ewram;
    std::vector<std::uint8_t> iwram;
    Ppu ppu{};
    Apu apu{};
    DmaController dma{};
    Timers timers{};
    std::uint16_t ie = 0;
    std::uint16_t if_flags = 0;
    std::uint16_t ime = 0;
    std::uint16_t waitcnt = 0;
    std::uint8_t postflg = 0;
    std::uint16_t keyinput = 0x3FFu;
    std::uint16_t keycnt = 0;
    bool halt_requested = false;
    bool stop_requested = false;
    std::uint32_t open_bus_latch = 0;
    std::uint32_t cpu_pc = 0x08000000u;

    Bus(Bios bios_in, Cartridge cart_in);
    void Reset();

    void SetKeys(std::uint32_t keys);

    [[nodiscard]] bool IrqPending() const {
        return (ime & 1u) != 0 && (ie & if_flags) != 0;
    }

    [[nodiscard]] bool AnyEnabledIrqFlagged() const {
        return (ie & if_flags) != 0;
    }

    bool Tick(std::uint32_t cycles);

    [[nodiscard]] std::uint8_t ReadU8(std::uint32_t addr);
    [[nodiscard]] std::uint16_t ReadU16(std::uint32_t addr);
    [[nodiscard]] std::uint32_t ReadU32(std::uint32_t addr);

    void WriteU8(std::uint32_t addr, std::uint8_t val);
    void WriteU16(std::uint32_t addr, std::uint16_t val);
    void WriteU32(std::uint32_t addr, std::uint32_t val);

    void RunDma(DmaTrigger trigger);

private:
    [[nodiscard]] std::uint16_t ReadIoU16(std::uint32_t addr) const;
    void WriteIoU8(std::uint32_t addr, std::uint8_t val);
    void WriteIoU16(std::uint32_t addr, std::uint16_t val);
    void WriteIoU32(std::uint32_t addr, std::uint32_t val);

    void RunFifoDma(std::size_t ch);
    void ExecuteDmaChannel(std::size_t ch);
};

} // namespace Emulation::Gba
