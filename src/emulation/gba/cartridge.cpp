// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "emulation/gba/cartridge.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string_view>

namespace Emulation::Gba {
namespace {

constexpr std::size_t HeaderMinSize = 0xC0;
constexpr std::array<std::uint8_t, 4> LogoPrefix = {0x24, 0xFF, 0xAE, 0x51};

std::uint8_t ReverseBits8(std::uint8_t b) {
    b = static_cast<std::uint8_t>(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
    b = static_cast<std::uint8_t>(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    b = static_cast<std::uint8_t>(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
    return b;
}

bool ContainsAscii(std::span<const std::uint8_t> haystack, std::string_view needle) {
    if (haystack.size() < needle.size()) {
        return false;
    }
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                [](std::uint8_t a, char b) { return a == static_cast<std::uint8_t>(b); });
    return it != haystack.end();
}

SaveType DetectSaveType(std::span<const std::uint8_t> rom, std::span<const std::uint8_t> save) {
    if (ContainsAscii(rom, "FLASH1M_V")) {
        return SaveType::Flash128K;
    }
    if (ContainsAscii(rom, "FLASH512_V") || ContainsAscii(rom, "FLASH_V")) {
        return SaveType::Flash64K;
    }
    if (ContainsAscii(rom, "EEPROM_V")) {
        return (save.size() > 512) ? SaveType::Eeprom64K : SaveType::Eeprom4K;
    }
    if (ContainsAscii(rom, "SRAM_V") || ContainsAscii(rom, "SRAM_F_V")) {
        return SaveType::Sram;
    }
    if (!save.empty()) {
        switch (save.size()) {
        case 512:
            return SaveType::Eeprom4K;
        case 8192:
            return SaveType::Eeprom64K;
        case 65536:
            return SaveType::Flash64K;
        case 131072:
            return SaveType::Flash128K;
        default:
            return SaveType::Sram;
        }
    }
    return SaveType::Sram;
}

void AppendU32Le(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void AppendU64Le(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
    }
}

std::uint32_t ReadU32Le(std::span<const std::uint8_t> s, std::size_t off) {
    return static_cast<std::uint32_t>(s[off]) | (static_cast<std::uint32_t>(s[off + 1]) << 8) |
           (static_cast<std::uint32_t>(s[off + 2]) << 16) |
           (static_cast<std::uint32_t>(s[off + 3]) << 24);
}

std::uint64_t ReadU64Le(std::span<const std::uint8_t> s, std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(s[off + static_cast<std::size_t>(i)]) << (i * 8);
    }
    return v;
}

} // namespace

std::uint16_t Rtc::ReadGpio(std::uint32_t addr) const {
    if ((control & 1u) == 0) {
        return 0;
    }
    switch (addr) {
    case 0x080000C4u:
        return data;
    case 0x080000C6u:
        return direction;
    case 0x080000C8u:
        return control;
    default:
        return 0;
    }
}

void Rtc::WriteGpio(std::uint32_t addr, std::uint16_t val) {
    switch (addr) {
    case 0x080000C4u:
        WriteData(static_cast<std::uint8_t>(val & 0xFu));
        break;
    case 0x080000C6u:
        direction = static_cast<std::uint8_t>(val & 0xFu);
        break;
    case 0x080000C8u:
        control = static_cast<std::uint8_t>(val & 0x1u);
        break;
    default:
        break;
    }
}

void Rtc::WriteData(std::uint8_t val) {
    const bool sck = (val & 0x01u) != 0;
    const bool sio_in = (val & 0x02u) != 0;
    const bool cs = (val & 0x04u) != 0;

    if (!cs) {
        last_cs_ = false;
        last_sck_ = sck;
        bit_count_ = 0;
        shift_byte_ = 0;
        command_ = 0;
        in_bytes_.clear();
        out_bytes_.clear();
        out_idx_ = 0;
        out_bit_ = 0;
        data = static_cast<std::uint8_t>((data & ~0x5u) | (val & 0x5u));
        return;
    }

    if (!last_cs_ && cs) {
        last_cs_ = true;
        last_sck_ = sck;
        bit_count_ = 0;
        shift_byte_ = 0;
        command_ = 0;
        in_bytes_.clear();
        out_bytes_.clear();
        out_idx_ = 0;
        out_bit_ = 0;
        return;
    }

    const bool rising = !last_sck_ && sck;
    const bool falling = last_sck_ && !sck;
    last_sck_ = sck;

    const bool sio_output = (direction & 0x02u) == 0;
    if (sio_output && falling) {
        if (out_idx_ < out_bytes_.size()) {
            const std::uint8_t bit = static_cast<std::uint8_t>((out_bytes_[out_idx_] >> out_bit_) & 1u);
            if (bit != 0) {
                data |= 0x02u;
            } else {
                data &= static_cast<std::uint8_t>(~0x02u);
            }
            ++out_bit_;
            if (out_bit_ == 8) {
                out_bit_ = 0;
                ++out_idx_;
            }
        } else {
            data &= static_cast<std::uint8_t>(~0x02u);
        }
    } else if (!sio_output && rising) {
        if (sio_in) {
            shift_byte_ |= static_cast<std::uint8_t>(1u << bit_count_);
        }
        ++bit_count_;
        if (bit_count_ == 8) {
            const std::uint8_t byte = shift_byte_;
            shift_byte_ = 0;
            bit_count_ = 0;
            HandleByte(byte);
        }
    }
    data = static_cast<std::uint8_t>((sio_output ? (data & 0x02u) : (val & 0x02u)) | (val & 0x05u));
}

void Rtc::HandleByte(std::uint8_t byte) {
    if (command_ == 0) {
        const std::uint8_t cmd = ((byte & 0xF0u) == 0x60u) ? byte : ReverseBits8(byte);
        command_ = cmd;
        const std::uint8_t reg = static_cast<std::uint8_t>((cmd >> 1) & 0x07u);
        const bool is_read = (cmd & 0x01u) != 0;
        if (is_read) {
            switch (reg) {
            case 0:
                out_bytes_.clear();
                break;
            case 1:
                out_bytes_ = {status_reg};
                break;
            case 2:
                out_bytes_ = CurrentDatetimeBcd();
                break;
            case 3: {
                auto dt = CurrentDatetimeBcd();
                out_bytes_.assign(dt.begin() + 4, dt.begin() + 7);
                break;
            }
            default:
                out_bytes_ = {0};
                break;
            }
            out_idx_ = 0;
            out_bit_ = 0;
        } else {
            in_bytes_.clear();
            if (reg == 0) {
                status_reg = 0x40;
                command_ = 0;
            }
        }
    } else {
        in_bytes_.push_back(byte);
        const std::uint8_t reg = static_cast<std::uint8_t>((command_ >> 1) & 0x07u);
        if (reg == 1 && !in_bytes_.empty()) {
            status_reg = static_cast<std::uint8_t>((in_bytes_[0] & 0x4Au) | 0x40u);
            command_ = 0;
        } else if ((reg == 2 && in_bytes_.size() >= 7) || (reg == 3 && in_bytes_.size() >= 3)) {
            command_ = 0;
        }
    }
}

std::vector<std::uint8_t> Rtc::CurrentDatetimeBcd() {
    const auto now = std::chrono::system_clock::now();
    const auto secs_signed =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const std::int64_t secs = (secs_signed > 0) ? secs_signed : 0;
    const std::int64_t days = secs / 86'400;
    const std::int64_t rem = secs % 86'400;
    const auto hour = static_cast<std::uint8_t>(rem / 3'600);
    const auto minute = static_cast<std::uint8_t>((rem % 3'600) / 60);
    const auto second = static_cast<std::uint8_t>(rem % 60);
    const auto dow = static_cast<std::uint8_t>((((days + 4) % 7) + 7) % 7);

    // Civil date from days since 1970-01-01 (Howard Hinnant's algorithm)
    const std::int64_t z = days + 719'468;
    const std::int64_t era = (z >= 0 ? z : z - 146'096) / 146'097;
    const auto doe = static_cast<std::uint64_t>(z - era * 146'097);
    const std::uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146'096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const std::uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::uint64_t mp = (5 * doy + 2) / 153;
    const auto d = static_cast<std::uint8_t>(doy - (153 * mp + 2) / 5 + 1);
    const auto m = static_cast<std::uint8_t>(mp < 10 ? mp + 3 : mp - 9);
    const std::int64_t year_full = y + (m <= 2 ? 1 : 0);
    const auto year = static_cast<std::uint8_t>(std::clamp<std::int64_t>(year_full - 2000, 0, 99));

    const auto to_bcd = [](std::uint8_t v) -> std::uint8_t {
        return static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10));
    };
    return {
        to_bcd(year), to_bcd(m), to_bcd(d), to_bcd(dow),
        to_bcd(hour), to_bcd(minute), to_bcd(second),
    };
}

bool Cartridge::ValidateRom(std::span<const std::uint8_t> rom, std::string& error) {
    if (rom.size() < HeaderMinSize) {
        error = "Invalid GBA ROM: ROM is smaller than the 192-byte GBA header";
        return false;
    }
    if (rom.size() > MaxRomBytes) {
        error = "Invalid GBA ROM: ROM exceeds 32 MiB limit";
        return false;
    }
    if (!std::equal(LogoPrefix.begin(), LogoPrefix.end(), rom.begin() + 0x04)) {
        error = "Invalid GBA ROM: Missing GBA logo signature";
        return false;
    }
    if (rom[0xB2] != 0x96u) {
        error = "Invalid GBA ROM: Invalid fixed header byte at 0xB2";
        return false;
    }
    std::uint8_t sum = 0x19u;
    for (std::size_t i = 0xA0; i < 0xBD; ++i) {
        sum = static_cast<std::uint8_t>(sum + rom[i]);
    }
    const std::uint8_t expected = static_cast<std::uint8_t>(0u - sum);
    if (rom[0xBD] != expected) {
        error = "Invalid GBA ROM: Invalid GBA header complement checksum";
        return false;
    }
    return true;
}

bool Cartridge::Create(std::vector<std::uint8_t> rom, std::span<const std::uint8_t> save,
                       Cartridge& out, std::string& error) {
    if (!ValidateRom(rom, error)) {
        return false;
    }
    if (save.size() > MaxSaveBytes) {
        error = "GBA battery save exceeds the size limit";
        return false;
    }

    Cartridge cart{};
    cart.save_type = DetectSaveType(rom, save);
    cart.rom_ = std::move(rom);
    cart.sram_.assign(32 * 1024, 0xFFu);
    cart.flash_.assign(128 * 1024, 0xFFu);
    cart.eeprom_.assign(8 * 1024, 0xFFu);
    cart.eeprom_bits_ = 14;

    switch (cart.save_type) {
    case SaveType::Sram: {
        const std::size_t n = (std::min)(save.size(), cart.sram_.size());
        std::copy_n(save.begin(), n, cart.sram_.begin());
        break;
    }
    case SaveType::Flash64K: {
        cart.flash_.assign(64 * 1024, 0xFFu);
        const std::size_t n = (std::min)(save.size(), cart.flash_.size());
        std::copy_n(save.begin(), n, cart.flash_.begin());
        break;
    }
    case SaveType::Flash128K: {
        cart.flash_.assign(128 * 1024, 0xFFu);
        const std::size_t n = (std::min)(save.size(), cart.flash_.size());
        std::copy_n(save.begin(), n, cart.flash_.begin());
        break;
    }
    case SaveType::Eeprom4K: {
        cart.eeprom_.assign(512, 0xFFu);
        cart.eeprom_bits_ = 6;
        const std::size_t n = (std::min)(save.size(), cart.eeprom_.size());
        std::copy_n(save.begin(), n, cart.eeprom_.begin());
        break;
    }
    case SaveType::Eeprom64K: {
        cart.eeprom_.assign(8 * 1024, 0xFFu);
        cart.eeprom_bits_ = 14;
        const std::size_t n = (std::min)(save.size(), cart.eeprom_.size());
        std::copy_n(save.begin(), n, cart.eeprom_.begin());
        break;
    }
    case SaveType::None:
        break;
    }

    out = std::move(cart);
    error.clear();
    return true;
}

std::uint8_t Cartridge::ReadRomU8(std::uint32_t offset) const {
    if (rtc.control != 0 && offset >= 0xC4u && offset <= 0xC9u) {
        const std::uint16_t half = rtc.ReadGpio(0x08000000u | (offset & ~1u));
        return ((offset & 1u) == 0) ? static_cast<std::uint8_t>(half & 0xFFu)
                                    : static_cast<std::uint8_t>(half >> 8);
    }
    const std::size_t off = static_cast<std::size_t>(offset & 0x01FFFFFFu);
    if (off < rom_.size()) {
        return rom_[off];
    }
    const std::uint16_t half = static_cast<std::uint16_t>((offset >> 1) & 0xFFFFu);
    return ((offset & 1u) == 0) ? static_cast<std::uint8_t>(half & 0xFFu)
                                : static_cast<std::uint8_t>(half >> 8);
}

std::uint16_t Cartridge::ReadRomU16(std::uint32_t offset) const {
    const std::uint32_t aligned = offset & ~1u;
    if (rtc.control != 0 && aligned >= 0xC4u && aligned <= 0xC8u) {
        return rtc.ReadGpio(0x08000000u | aligned);
    }
    const std::size_t off = static_cast<std::size_t>(aligned & 0x01FFFFFFu);
    if (off + 1 < rom_.size()) {
        return static_cast<std::uint16_t>(rom_[off]) |
               (static_cast<std::uint16_t>(rom_[off + 1]) << 8);
    }
    if (off < rom_.size()) {
        return static_cast<std::uint16_t>(rom_[off]);
    }
    return static_cast<std::uint16_t>((aligned >> 1) & 0xFFFFu);
}

std::uint32_t Cartridge::ReadRomU32(std::uint32_t offset) const {
    const std::uint32_t aligned = offset & ~3u;
    const std::uint32_t lo = ReadRomU16(aligned);
    const std::uint32_t hi = ReadRomU16(aligned + 2u);
    return lo | (hi << 16);
}

void Cartridge::WriteGpio(std::uint32_t offset, std::uint16_t val) {
    const std::uint32_t aligned = offset & ~1u;
    if (aligned >= 0xC4u && aligned <= 0xC8u) {
        rtc.WriteGpio(0x08000000u | aligned, val);
    }
}

std::uint8_t Cartridge::ReadBackupU8(std::uint32_t offset) const {
    switch (save_type) {
    case SaveType::Sram:
    case SaveType::None:
        return sram_[static_cast<std::size_t>(offset) & (sram_.size() - 1u)];
    case SaveType::Flash64K:
    case SaveType::Flash128K: {
        const std::size_t off = static_cast<std::size_t>(offset) & 0xFFFFu;
        if (flash_id_mode_) {
            if (off == 0) {
                return (save_type == SaveType::Flash128K) ? 0x62u : 0x32u;
            }
            if (off == 1) {
                return (save_type == SaveType::Flash128K) ? 0x13u : 0x1Bu;
            }
            return 0xFFu;
        }
        const std::size_t bank_off = (save_type == SaveType::Flash128K) ? (flash_bank_ * 0x10000u) : 0u;
        return flash_[bank_off + off];
    }
    case SaveType::Eeprom4K:
    case SaveType::Eeprom64K:
        return 1;
    }
    return 0xFFu;
}

void Cartridge::WriteBackupU8(std::uint32_t offset, std::uint8_t val) {
    switch (save_type) {
    case SaveType::Sram:
        sram_[static_cast<std::size_t>(offset) & (sram_.size() - 1u)] = val;
        break;
    case SaveType::Flash64K:
    case SaveType::Flash128K:
        WriteFlash(static_cast<std::size_t>(offset) & 0xFFFFu, val);
        break;
    case SaveType::None:
    case SaveType::Eeprom4K:
    case SaveType::Eeprom64K:
        break;
    }
}

void Cartridge::WriteFlash(std::size_t off, std::uint8_t val) {
    switch (flash_state_) {
    case FlashState::WriteByte: {
        const std::size_t bank_off = (save_type == SaveType::Flash128K) ? (flash_bank_ * 0x10000u) : 0u;
        flash_[bank_off + off] &= val;
        flash_state_ = FlashState::Ready;
        break;
    }
    case FlashState::BankSwitch:
        if (off == 0 && save_type == SaveType::Flash128K) {
            flash_bank_ = static_cast<std::size_t>(val & 1u);
        }
        flash_state_ = FlashState::Ready;
        break;
    case FlashState::Ready:
        if (off == 0x5555u && val == 0xAAu) {
            flash_state_ = FlashState::Cmd1;
        }
        break;
    case FlashState::Cmd1:
        if (off == 0x2AAAu && val == 0x55u) {
            flash_state_ = FlashState::Cmd2;
        } else {
            flash_state_ = FlashState::Ready;
        }
        break;
    case FlashState::Cmd2:
        if (off == 0x5555u) {
            switch (val) {
            case 0x90u:
                flash_id_mode_ = true;
                flash_state_ = FlashState::Ready;
                break;
            case 0xF0u:
                flash_id_mode_ = false;
                flash_state_ = FlashState::Ready;
                break;
            case 0x80u:
                flash_state_ = FlashState::EraseCmd1;
                break;
            case 0xA0u:
                flash_state_ = FlashState::WriteByte;
                break;
            case 0xB0u:
                flash_state_ = FlashState::BankSwitch;
                break;
            default:
                flash_state_ = FlashState::Ready;
                break;
            }
        } else {
            flash_state_ = FlashState::Ready;
        }
        break;
    case FlashState::EraseCmd1:
        if (off == 0x5555u && val == 0xAAu) {
            flash_state_ = FlashState::EraseCmd2;
        } else {
            flash_state_ = FlashState::Ready;
        }
        break;
    case FlashState::EraseCmd2:
        if (off == 0x2AAAu && val == 0x55u) {
            flash_state_ = FlashState::EraseCmd2;
        } else if (off == 0x5555u && val == 0x10u) {
            std::fill(flash_.begin(), flash_.end(), 0xFFu);
            flash_state_ = FlashState::Ready;
        } else if (val == 0x30u) {
            const std::size_t bank_off =
                (save_type == SaveType::Flash128K) ? (flash_bank_ * 0x10000u) : 0u;
            const std::size_t sector_start = bank_off + (off & 0xF000u);
            std::fill_n(flash_.begin() + static_cast<std::ptrdiff_t>(sector_start), 0x1000, 0xFFu);
            flash_state_ = FlashState::Ready;
        } else {
            flash_state_ = FlashState::Ready;
        }
        break;
    }
}

bool Cartridge::IsEepromAccess(std::uint32_t addr) const {
    if (save_type != SaveType::Eeprom4K && save_type != SaveType::Eeprom64K) {
        return false;
    }
    if (rom_.size() > 16 * 1024 * 1024) {
        return addr >= 0x0DFFFF00u;
    }
    return addr >= 0x0D000000u && addr < 0x0E000000u;
}

void Cartridge::ConfigureEepromDma(std::uint32_t transfer_count) {
    if (save_type == SaveType::Eeprom4K && (transfer_count == 17u || transfer_count == 81u)) {
        eeprom_bits_ = 14;
        save_type = SaveType::Eeprom64K;
        eeprom_.resize(8 * 1024, 0xFFu);
    }
}

std::uint16_t Cartridge::ReadEeprom() {
    switch (eeprom_phase_) {
    case EepromPhase::ReadWait:
        ++eeprom_bit_idx_;
        if (eeprom_bit_idx_ >= 4) {
            eeprom_phase_ = EepromPhase::ReadBits;
            eeprom_bit_idx_ = 0;
        }
        return 0;
    case EepromPhase::ReadBits: {
        if (eeprom_bit_idx_ >= 64 || eeprom_.empty()) {
            eeprom_phase_ = EepromPhase::Idle;
            eeprom_bit_idx_ = 0;
            return 0;
        }
        const std::size_t addr_mask = (eeprom_bits_ == 14) ? 0x3FFu : 0x3Fu;
        const std::size_t max_off = eeprom_.size() >= 8 ? (eeprom_.size() - 8) : 0;
        const std::size_t byte_addr = (std::min)((eeprom_addr_ & addr_mask) * 8, max_off);
        const std::size_t byte_idx = byte_addr + (eeprom_bit_idx_ / 8);
        const std::uint8_t byte = (byte_idx < eeprom_.size()) ? eeprom_[byte_idx] : 0xFFu;
        const std::uint8_t bit =
            static_cast<std::uint8_t>((byte >> (7u - (eeprom_bit_idx_ % 8u))) & 1u);
        ++eeprom_bit_idx_;
        if (eeprom_bit_idx_ >= 64) {
            eeprom_phase_ = EepromPhase::Idle;
            eeprom_bit_idx_ = 0;
        }
        return bit;
    }
    default:
        return eeprom_ready_ ? 1u : 0u;
    }
}

void Cartridge::WriteEeprom(std::uint16_t val) {
    const std::uint64_t bit = static_cast<std::uint64_t>(val & 1u);
    switch (eeprom_phase_) {
    case EepromPhase::Idle:
        eeprom_cmd_ = static_cast<std::uint8_t>(bit);
        eeprom_bit_idx_ = 1;
        eeprom_phase_ = EepromPhase::Command;
        break;
    case EepromPhase::Command:
        eeprom_cmd_ = static_cast<std::uint8_t>((eeprom_cmd_ << 1) | bit);
        eeprom_addr_ = 0;
        eeprom_addr_bits_ = 0;
        eeprom_phase_ = EepromPhase::Address;
        break;
    case EepromPhase::Address:
        eeprom_addr_ = (eeprom_addr_ << 1) | static_cast<std::size_t>(bit);
        ++eeprom_addr_bits_;
        if (eeprom_addr_bits_ >= eeprom_bits_) {
            if (eeprom_cmd_ == 0b11u) {
                eeprom_phase_ = EepromPhase::ReadWait;
                eeprom_bit_idx_ = 0;
            } else if (eeprom_cmd_ == 0b10u) {
                eeprom_phase_ = EepromPhase::WriteData;
                eeprom_bit_idx_ = 0;
                eeprom_write_val_ = 0;
            } else {
                eeprom_phase_ = EepromPhase::Idle;
            }
        }
        break;
    case EepromPhase::WriteData:
        eeprom_write_val_ = (eeprom_write_val_ << 1) | bit;
        ++eeprom_bit_idx_;
        if (eeprom_bit_idx_ >= 64) {
            const std::size_t addr_mask = (eeprom_bits_ == 14) ? 0x3FFu : 0x3Fu;
            const std::size_t byte_addr = (eeprom_addr_ & addr_mask) * 8;
            if (byte_addr + 8 <= eeprom_.size()) {
                for (int i = 0; i < 8; ++i) {
                    eeprom_[byte_addr + static_cast<std::size_t>(i)] =
                        static_cast<std::uint8_t>((eeprom_write_val_ >> ((7 - i) * 8)) & 0xFFu);
                }
            }
            eeprom_phase_ = EepromPhase::WriteWait;
        }
        break;
    case EepromPhase::WriteWait:
    case EepromPhase::ReadWait:
    case EepromPhase::ReadBits:
        if (eeprom_phase_ == EepromPhase::WriteWait) {
            eeprom_phase_ = EepromPhase::Idle;
            eeprom_ready_ = true;
        }
        break;
    }
}

std::vector<std::uint8_t> Cartridge::SaveBytes() const {
    switch (save_type) {
    case SaveType::None:
        return {};
    case SaveType::Sram:
        return sram_;
    case SaveType::Flash64K:
        return std::vector<std::uint8_t>(flash_.begin(), flash_.begin() + 64 * 1024);
    case SaveType::Flash128K:
        return flash_;
    case SaveType::Eeprom4K:
        return std::vector<std::uint8_t>(eeprom_.begin(), eeprom_.begin() + 512);
    case SaveType::Eeprom64K:
        return std::vector<std::uint8_t>(eeprom_.begin(), eeprom_.begin() + 8 * 1024);
    }
    return {};
}

std::vector<std::uint8_t> Cartridge::StateSnapshot() const {
    auto save = SaveBytes();
    std::vector<std::uint8_t> out;
    out.reserve(save.size() + 32);
    out.push_back(static_cast<std::uint8_t>(save_type));
    out.push_back(static_cast<std::uint8_t>(flash_state_));
    out.push_back(flash_id_mode_ ? 1u : 0u);
    out.push_back(static_cast<std::uint8_t>(flash_bank_));
    out.push_back(static_cast<std::uint8_t>(eeprom_bits_));
    out.push_back(static_cast<std::uint8_t>(eeprom_phase_));
    out.push_back(eeprom_cmd_);
    AppendU32Le(out, static_cast<std::uint32_t>(eeprom_addr_));
    out.push_back(static_cast<std::uint8_t>(eeprom_addr_bits_));
    out.push_back(static_cast<std::uint8_t>(eeprom_bit_idx_));
    AppendU64Le(out, eeprom_write_val_);
    out.push_back(eeprom_ready_ ? 1u : 0u);
    out.push_back(rtc.data);
    out.push_back(rtc.direction);
    out.push_back(rtc.control);
    out.push_back(rtc.status_reg);
    AppendU32Le(out, static_cast<std::uint32_t>(save.size()));
    out.insert(out.end(), save.begin(), save.end());
    out.push_back(rtc.last_sck_ ? 1u : 0u);
    out.push_back(rtc.last_cs_ ? 1u : 0u);
    out.push_back(rtc.bit_count_);
    out.push_back(rtc.shift_byte_);
    out.push_back(rtc.command_);
    out.push_back(static_cast<std::uint8_t>((std::min<std::size_t>)(rtc.in_bytes_.size(), 16u)));
    for (std::size_t i = 0; i < (std::min<std::size_t>)(rtc.in_bytes_.size(), 16u); ++i) {
        out.push_back(rtc.in_bytes_[i]);
    }
    out.push_back(static_cast<std::uint8_t>((std::min<std::size_t>)(rtc.out_bytes_.size(), 16u)));
    for (std::size_t i = 0; i < (std::min<std::size_t>)(rtc.out_bytes_.size(), 16u); ++i) {
        out.push_back(rtc.out_bytes_[i]);
    }
    AppendU32Le(out, static_cast<std::uint32_t>(rtc.out_idx_));
    out.push_back(rtc.out_bit_);
    return out;
}

bool Cartridge::RestoreSnapshot(std::span<const std::uint8_t> data, std::string& error) {
    if (data.size() < 30) {
        error = "Invalid GBA save state: Truncated cartridge state";
        return false;
    }
    SaveType next_save_type = SaveType::None;
    if (!SaveTypeFromU8(data[0], next_save_type)) {
        error = "Invalid GBA save state: Invalid save type";
        return false;
    }
    if (data[1] > static_cast<std::uint8_t>(FlashState::BankSwitch)) {
        error = "Invalid GBA save state: Invalid flash state";
        return false;
    }
    if (data[4] != 6u && data[4] != 14u) {
        error = "Invalid GBA save state: Invalid EEPROM address width";
        return false;
    }
    if (data[5] > static_cast<std::uint8_t>(EepromPhase::WriteWait)) {
        error = "Invalid GBA save state: Invalid EEPROM phase";
        return false;
    }
    if (data[11] > data[4] || data[12] >= 64u) {
        error = "Invalid GBA save state: Invalid EEPROM bit index";
        return false;
    }
    const std::size_t save_len = ReadU32Le(data, 26);
    if (save_len > MaxSaveBytes || data.size() < 30 + save_len) {
        error = "Invalid GBA save state: Corrupted cartridge save length";
        return false;
    }

    std::size_t tail = 30 + save_len;
    if (data.size() < tail + 12) {
        error = "Invalid GBA save state: Truncated RTC state";
        return false;
    }
    const bool rtc_last_sck = data[tail++] != 0;
    const bool rtc_last_cs = data[tail++] != 0;
    const std::uint8_t rtc_bit_count = data[tail++];
    const std::uint8_t rtc_shift_byte = data[tail++];
    const std::uint8_t rtc_command = data[tail++];
    const std::size_t rtc_in_len = data[tail++];
    if (rtc_bit_count >= 8u || rtc_in_len > 16u || data.size() < tail + rtc_in_len + 6) {
        error = "Invalid GBA save state: Corrupted RTC input buffer";
        return false;
    }
    std::vector<std::uint8_t> rtc_in(data.begin() + static_cast<std::ptrdiff_t>(tail),
                                     data.begin() + static_cast<std::ptrdiff_t>(tail + rtc_in_len));
    tail += rtc_in_len;
    const std::size_t rtc_out_len = data[tail++];
    if (rtc_out_len > 16u || data.size() < tail + rtc_out_len + 5) {
        error = "Invalid GBA save state: Corrupted RTC output buffer";
        return false;
    }
    std::vector<std::uint8_t> rtc_out(data.begin() + static_cast<std::ptrdiff_t>(tail),
                                      data.begin() + static_cast<std::ptrdiff_t>(tail + rtc_out_len));
    tail += rtc_out_len;
    const std::size_t rtc_out_idx = ReadU32Le(data, tail);
    tail += 4;
    const std::uint8_t rtc_out_bit = data[tail++];
    if (rtc_out_idx > rtc_out.size() || rtc_out_bit >= 8u || tail != data.size()) {
        error = "Invalid GBA save state: Corrupted RTC output cursor";
        return false;
    }

    const auto save = data.subspan(30, save_len);
    save_type = next_save_type;
    flash_state_ = static_cast<FlashState>(data[1]);
    flash_id_mode_ = data[2] != 0;
    flash_bank_ = data[3] & 1u;
    eeprom_bits_ = data[4];
    eeprom_phase_ = static_cast<EepromPhase>(data[5]);
    eeprom_cmd_ = data[6];
    eeprom_addr_ = ReadU32Le(data, 7);
    eeprom_addr_bits_ = data[11];
    eeprom_bit_idx_ = data[12];
    eeprom_write_val_ = ReadU64Le(data, 13);
    eeprom_ready_ = data[21] != 0;
    rtc.data = data[22];
    rtc.direction = data[23];
    rtc.control = data[24];
    rtc.status_reg = data[25];
    rtc.last_sck_ = rtc_last_sck;
    rtc.last_cs_ = rtc_last_cs;
    rtc.bit_count_ = rtc_bit_count;
    rtc.shift_byte_ = rtc_shift_byte;
    rtc.command_ = rtc_command;
    rtc.in_bytes_ = std::move(rtc_in);
    rtc.out_bytes_ = std::move(rtc_out);
    rtc.out_idx_ = rtc_out_idx;
    rtc.out_bit_ = rtc_out_bit;

    switch (save_type) {
    case SaveType::None:
        break;
    case SaveType::Sram: {
        std::fill(sram_.begin(), sram_.end(), 0xFFu);
        const std::size_t n = (std::min)(save.size(), sram_.size());
        std::copy_n(save.begin(), n, sram_.begin());
        break;
    }
    case SaveType::Flash64K: {
        flash_.assign(64 * 1024, 0xFFu);
        const std::size_t n = (std::min)(save.size(), flash_.size());
        std::copy_n(save.begin(), n, flash_.begin());
        break;
    }
    case SaveType::Flash128K: {
        flash_.assign(128 * 1024, 0xFFu);
        const std::size_t n = (std::min)(save.size(), flash_.size());
        std::copy_n(save.begin(), n, flash_.begin());
        break;
    }
    case SaveType::Eeprom4K: {
        eeprom_.assign(512, 0xFFu);
        const std::size_t n = (std::min)(save.size(), eeprom_.size());
        std::copy_n(save.begin(), n, eeprom_.begin());
        break;
    }
    case SaveType::Eeprom64K: {
        eeprom_.assign(8 * 1024, 0xFFu);
        const std::size_t n = (std::min)(save.size(), eeprom_.size());
        std::copy_n(save.begin(), n, eeprom_.begin());
        break;
    }
    }
    return true;
}

} // namespace Emulation::Gba
