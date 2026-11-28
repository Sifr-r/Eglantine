// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "emulation/gba/types.h"

namespace Emulation::Gba {

enum class FlashState : std::uint8_t {
    Ready = 0,
    Cmd1 = 1,
    Cmd2 = 2,
    EraseCmd1 = 3,
    EraseCmd2 = 4,
    WriteByte = 5,
    BankSwitch = 6,
};

enum class EepromPhase : std::uint8_t {
    Idle = 0,
    Command = 1,
    Address = 2,
    ReadWait = 3,
    ReadBits = 4,
    WriteData = 5,
    WriteWait = 6,
};

class Rtc {
public:
    std::uint8_t data = 0;
    std::uint8_t direction = 0;
    std::uint8_t control = 0;
    std::uint8_t status_reg = 0x40; // 24-hour mode enabled

    [[nodiscard]] std::uint16_t ReadGpio(std::uint32_t addr) const;
    void WriteGpio(std::uint32_t addr, std::uint16_t val);

private:
    void WriteData(std::uint8_t val);
    void HandleByte(std::uint8_t byte);
    [[nodiscard]] static std::vector<std::uint8_t> CurrentDatetimeBcd();

    bool last_sck_ = false;
    bool last_cs_ = false;
    std::uint8_t bit_count_ = 0;
    std::uint8_t shift_byte_ = 0;
    std::uint8_t command_ = 0;
    std::vector<std::uint8_t> in_bytes_;
    std::vector<std::uint8_t> out_bytes_;
    std::size_t out_idx_ = 0;
    std::uint8_t out_bit_ = 0;

    friend class Cartridge;
};

class Cartridge {
public:
    SaveType save_type = SaveType::None;
    Rtc rtc{};

    [[nodiscard]] static bool ValidateRom(std::span<const std::uint8_t> rom, std::string& error);
    [[nodiscard]] static bool Create(std::vector<std::uint8_t> rom,
                                     std::span<const std::uint8_t> save, Cartridge& out,
                                     std::string& error);

    [[nodiscard]] std::span<const std::uint8_t> Rom() const {
        return rom_;
    }

    [[nodiscard]] std::uint8_t ReadRomU8(std::uint32_t offset) const;
    [[nodiscard]] std::uint16_t ReadRomU16(std::uint32_t offset) const;
    [[nodiscard]] std::uint32_t ReadRomU32(std::uint32_t offset) const;
    void WriteGpio(std::uint32_t offset, std::uint16_t val);

    [[nodiscard]] std::uint8_t ReadBackupU8(std::uint32_t offset) const;
    void WriteBackupU8(std::uint32_t offset, std::uint8_t val);

    [[nodiscard]] bool IsEepromAccess(std::uint32_t addr) const;
    void ConfigureEepromDma(std::uint32_t transfer_count);
    [[nodiscard]] std::uint16_t ReadEeprom();
    void WriteEeprom(std::uint16_t val);

    [[nodiscard]] std::vector<std::uint8_t> SaveBytes() const;
    [[nodiscard]] std::vector<std::uint8_t> StateSnapshot() const;
    [[nodiscard]] bool RestoreSnapshot(std::span<const std::uint8_t> data, std::string& error);

private:
    void WriteFlash(std::size_t off, std::uint8_t val);

    std::vector<std::uint8_t> rom_;
    std::vector<std::uint8_t> sram_;
    std::vector<std::uint8_t> flash_;
    FlashState flash_state_ = FlashState::Ready;
    bool flash_id_mode_ = false;
    std::size_t flash_bank_ = 0;
    std::vector<std::uint8_t> eeprom_;
    std::size_t eeprom_bits_ = 14;
    EepromPhase eeprom_phase_ = EepromPhase::Idle;
    std::uint8_t eeprom_cmd_ = 0;
    std::size_t eeprom_addr_ = 0;
    std::size_t eeprom_addr_bits_ = 0;
    std::size_t eeprom_bit_idx_ = 0;
    std::uint64_t eeprom_write_val_ = 0;
    bool eeprom_ready_ = true;
};

} // namespace Emulation::Gba
