// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "emulation/gba/bios.h"
#include "emulation/gba/bus.h"
#include "emulation/gba/cpu_regs.h"
#include "emulation/gba/types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace Emulation::Gba {
namespace {

constexpr std::uint32_t BiosIfAddr = 0x03007FF8u;
constexpr std::uint32_t UserIrqPtrAddr = 0x03007FFCu;
constexpr double Pi = 3.14159265358979323846;
constexpr double FracPi2 = Pi * 0.5;
constexpr double Tau = Pi * 2.0;

void BitUnPack(std::uint32_t src, std::uint32_t dst, std::uint32_t info, Bus& bus) {
    const std::uint32_t len = bus.ReadU16(info);
    const std::uint8_t src_width = bus.ReadU8(info + 2u);
    const std::uint8_t dst_width = bus.ReadU8(info + 3u);
    const std::uint32_t data_off = bus.ReadU32(info + 4u);
    const bool zero_flag = (data_off & (1u << 31)) != 0;
    const std::uint32_t offset = data_off & 0x7FFFFFFFu;
    if ((src_width != 1u && src_width != 2u && src_width != 4u && src_width != 8u) ||
        (dst_width != 1u && dst_width != 2u && dst_width != 4u && dst_width != 8u &&
         dst_width != 16u && dst_width != 32u)) {
        return;
    }

    std::uint32_t out_word = 0;
    std::uint32_t out_bits = 0;
    const std::uint32_t src_mask = (1u << src_width) - 1u;
    for (std::uint32_t i = 0; i < len; ++i) {
        const std::uint32_t byte = bus.ReadU8(src);
        src += 1u;
        std::uint32_t bit = 0;
        while (bit < 8u) {
            std::uint32_t val = (byte >> bit) & src_mask;
            if (val != 0 || zero_flag) {
                val += offset;
            }
            out_word |= val << out_bits;
            out_bits += dst_width;
            if (out_bits >= 32u) {
                bus.WriteU32(dst, out_word);
                dst += 4u;
                out_word = 0;
                out_bits = 0;
            }
            bit += src_width;
        }
    }
}

void Lz77Decomp(std::uint32_t src, std::uint32_t dst, bool vram_16bit, Bus& bus) {
    const std::uint32_t header = bus.ReadU32(src);
    src += 4u;
    const std::size_t total = header >> 8;
    std::vector<std::uint8_t> out;
    out.reserve(total);

    while (out.size() < total) {
        const std::uint8_t flags = bus.ReadU8(src);
        src += 1u;
        for (int bit = 7; bit >= 0; --bit) {
            if (out.size() >= total) {
                break;
            }
            if ((flags & (1u << bit)) == 0) {
                out.push_back(bus.ReadU8(src));
                src += 1u;
            } else {
                const std::uint8_t b1 = bus.ReadU8(src);
                const std::uint8_t b2 = bus.ReadU8(src + 1u);
                src += 2u;
                const std::size_t len = static_cast<std::size_t>(b1 >> 4) + 3u;
                const std::size_t disp =
                    ((static_cast<std::size_t>(b1 & 0xFu) << 8) | static_cast<std::size_t>(b2)) + 1u;
                for (std::size_t k = 0; k < len; ++k) {
                    if (out.size() >= total) {
                        break;
                    }
                    const std::uint8_t byte = (out.size() >= disp) ? out[out.size() - disp] : 0u;
                    out.push_back(byte);
                }
            }
        }
    }

    if (vram_16bit) {
        for (std::size_t i = 0; i < out.size(); i += 2) {
            const std::uint16_t lo = out[i];
            const std::uint16_t hi = (i + 1 < out.size()) ? out[i + 1] : 0u;
            bus.WriteU16(dst, static_cast<std::uint16_t>(lo | (hi << 8)));
            dst += 2u;
        }
    } else {
        for (std::uint8_t b : out) {
            bus.WriteU8(dst, b);
            dst += 1u;
        }
    }
}

void RlDecomp(std::uint32_t src, std::uint32_t dst, bool vram_16bit, Bus& bus) {
    const std::uint32_t header = bus.ReadU32(src);
    src += 4u;
    const std::size_t total = header >> 8;
    std::vector<std::uint8_t> out;
    out.reserve(total);

    while (out.size() < total) {
        const std::uint8_t flag = bus.ReadU8(src);
        src += 1u;
        if ((flag & 0x80u) != 0) {
            const std::size_t count = static_cast<std::size_t>(flag & 0x7Fu) + 3u;
            const std::uint8_t b = bus.ReadU8(src);
            src += 1u;
            for (std::size_t k = 0; k < count; ++k) {
                if (out.size() < total) {
                    out.push_back(b);
                }
            }
        } else {
            const std::size_t count = static_cast<std::size_t>(flag & 0x7Fu) + 1u;
            for (std::size_t k = 0; k < count; ++k) {
                const std::uint8_t b = bus.ReadU8(src);
                src += 1u;
                if (out.size() < total) {
                    out.push_back(b);
                }
            }
        }
    }

    if (vram_16bit) {
        for (std::size_t i = 0; i < out.size(); i += 2) {
            const std::uint16_t lo = out[i];
            const std::uint16_t hi = (i + 1 < out.size()) ? out[i + 1] : 0u;
            bus.WriteU16(dst, static_cast<std::uint16_t>(lo | (hi << 8)));
            dst += 2u;
        }
    } else {
        for (std::uint8_t b : out) {
            bus.WriteU8(dst, b);
            dst += 1u;
        }
    }
}

} // namespace

bool Bios::Create(std::span<const std::uint8_t> bytes, Bios& out, std::string& error) {
    out = Bios{};
    if (!bytes.empty()) {
        if (bytes.size() != BiosBytes) {
            error = "Invalid GBA BIOS: Expected 16384 bytes, got " + std::to_string(bytes.size());
            return false;
        }
        out.data_.assign(bytes.begin(), bytes.end());
    }
    error.clear();
    return true;
}

std::uint8_t Bios::ReadU8(std::uint32_t addr, std::uint32_t pc) {
    const std::uint32_t word = ReadU32(addr & ~3u, pc);
    return static_cast<std::uint8_t>((word >> ((addr & 3u) * 8u)) & 0xFFu);
}

std::uint16_t Bios::ReadU16(std::uint32_t addr, std::uint32_t pc) {
    const std::uint32_t word = ReadU32(addr & ~3u, pc);
    return static_cast<std::uint16_t>((word >> ((addr & 2u) * 8u)) & 0xFFFFu);
}

std::uint32_t Bios::ReadU32(std::uint32_t addr, std::uint32_t pc) {
    const std::size_t aligned = addr & ~3u;
    if (!data_.empty() && pc < 0x00004000u && aligned + 3u < data_.size()) {
        const std::uint32_t val =
            static_cast<std::uint32_t>(data_[aligned]) |
            (static_cast<std::uint32_t>(data_[aligned + 1u]) << 8) |
            (static_cast<std::uint32_t>(data_[aligned + 2u]) << 16) |
            (static_cast<std::uint32_t>(data_[aligned + 3u]) << 24);
        latch = val;
        return val;
    }
    return latch;
}

void EnterHleIrq(CpuRegs& regs, Bus& bus, std::uint32_t return_lr) {
    const std::uint32_t old_cpsr = regs.cpsr;
    regs.SwitchMode(CpuMode::Irq);
    regs.spsr_irq = old_cpsr;
    regs.cpsr = (regs.cpsr & ~FlagT) | FlagI;
    regs.r[14] = return_lr;

    std::uint32_t sp = regs.r[13];
    for (std::size_t reg : {std::size_t{14}, std::size_t{12}, std::size_t{3}, std::size_t{2},
                            std::size_t{1}, std::size_t{0}}) {
        sp -= 4u;
        bus.WriteU32(sp, regs.r[reg]);
    }
    regs.r[13] = sp;
    regs.r[14] = HleIrqReturnAddr;

    const std::uint32_t handler = bus.ReadU32(UserIrqPtrAddr);
    if (handler == 0) {
        FinishHleIrq(regs, bus);
    } else {
        if ((handler & 1u) != 0) {
            regs.cpsr |= FlagT;
        }
        regs.r[15] = handler & ~1u;
    }
}

void FinishHleIrq(CpuRegs& regs, Bus& bus) {
    std::uint32_t sp = regs.r[13];
    for (std::size_t reg : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3},
                            std::size_t{12}, std::size_t{14}}) {
        regs.r[reg] = bus.ReadU32(sp);
        sp += 4u;
    }
    regs.r[13] = sp;
    const std::uint32_t spsr = regs.Spsr();
    const std::uint32_t target_pc = regs.r[14] - 4u;
    regs.SetCpsr(spsr);
    regs.r[15] = target_pc;
}

void HleSwi(std::uint8_t comment, CpuRegs& regs, Bus& bus) {
    switch (comment) {
    case 0x00: {
        const std::uint8_t flag = bus.ReadU8(0x03007FFAu);
        for (std::uint32_t addr = 0x03007E00u; addr < 0x03008000u; addr += 4u) {
            bus.WriteU32(addr, 0);
        }
        regs.sp_usr = 0x03007F00u;
        regs.sp_irq = 0x03007FA0u;
        regs.sp_svc = 0x03007FE0u;
        regs.SetCpsr(0x1Fu);
        regs.r.fill(0);
        regs.r[13] = regs.sp_usr;
        regs.r[15] = (flag != 0) ? 0x02000000u : 0x08000000u;
        break;
    }
    case 0x01: {
        const std::uint32_t flags = regs.r[0];
        if ((flags & (1u << 0)) != 0) {
            std::fill(bus.ewram.begin(), bus.ewram.end(), 0);
        }
        if ((flags & (1u << 1)) != 0) {
            std::fill_n(bus.iwram.begin(), 0x7E00, 0);
        }
        if ((flags & (1u << 2)) != 0) {
            std::fill(bus.ppu.palette_ram.begin(), bus.ppu.palette_ram.end(), 0);
        }
        if ((flags & (1u << 3)) != 0) {
            std::fill(bus.ppu.vram.begin(), bus.ppu.vram.end(), 0);
        }
        if ((flags & (1u << 4)) != 0) {
            std::fill(bus.ppu.oam.begin(), bus.ppu.oam.end(), 0);
        }
        if ((flags & (1u << 6)) != 0) {
            bus.apu.Reset();
        }
        break;
    }
    case 0x02:
    case 0x03:
        regs.halted = true;
        break;
    case 0x04:
    case 0x05: {
        const std::uint16_t mask =
            (comment == 0x05) ? std::uint16_t{0x0001} : static_cast<std::uint16_t>(regs.r[1] & 0x3FFFu);
        std::uint16_t bios_if = bus.ReadU16(BiosIfAddr);
        if (!bus.bios.intr_wait_active) {
            const bool discard_old = (comment == 0x05) ? true : (regs.r[0] != 0);
            if (discard_old) {
                bios_if = static_cast<std::uint16_t>(bios_if & ~mask);
                bus.WriteU16(BiosIfAddr, bios_if);
            }
        }
        if ((bios_if & mask) != 0) {
            bus.WriteU16(BiosIfAddr, static_cast<std::uint16_t>(bios_if & ~mask));
            bus.bios.intr_wait_active = false;
            bus.bios.intr_wait_mask = 0;
        } else {
            bus.bios.intr_wait_active = true;
            bus.bios.intr_wait_mask = mask;
            bus.ime = 1;
            bus.ie |= mask;
            regs.cpsr &= ~FlagI;
            regs.r[0] = 0;
            regs.r[1] = mask;
            regs.halted = true;
            const std::uint32_t step = regs.IsThumb() ? 2u : 4u;
            regs.r[15] -= step;
        }
        break;
    }
    case 0x06: {
        const auto num = static_cast<std::int32_t>(regs.r[0]);
        const auto den = static_cast<std::int32_t>(regs.r[1]);
        if (den != 0) {
            const std::int64_t q64 = static_cast<std::int64_t>(num) / static_cast<std::int64_t>(den);
            const std::int64_t r64 = static_cast<std::int64_t>(num) % static_cast<std::int64_t>(den);
            const auto q = static_cast<std::int32_t>(q64);
            regs.r[0] = static_cast<std::uint32_t>(q);
            regs.r[1] = static_cast<std::uint32_t>(static_cast<std::int32_t>(r64));
            regs.r[3] = static_cast<std::uint32_t>(q64 < 0 ? -q64 : q64);
        }
        break;
    }
    case 0x07: {
        const auto num = static_cast<std::int32_t>(regs.r[1]);
        const auto den = static_cast<std::int32_t>(regs.r[0]);
        if (den != 0) {
            const std::int64_t q64 = static_cast<std::int64_t>(num) / static_cast<std::int64_t>(den);
            const std::int64_t r64 = static_cast<std::int64_t>(num) % static_cast<std::int64_t>(den);
            const auto q = static_cast<std::int32_t>(q64);
            regs.r[0] = static_cast<std::uint32_t>(q);
            regs.r[1] = static_cast<std::uint32_t>(static_cast<std::int32_t>(r64));
            regs.r[3] = static_cast<std::uint32_t>(q64 < 0 ? -q64 : q64);
        }
        break;
    }
    case 0x08: {
        const auto x = static_cast<double>(regs.r[0]);
        regs.r[0] = static_cast<std::uint32_t>(std::sqrt(x));
        break;
    }
    case 0x09: {
        const double a = static_cast<double>(static_cast<std::int16_t>(regs.r[0])) / 16384.0;
        const double res = (std::atan(a) / FracPi2) * 16384.0;
        regs.r[0] = static_cast<std::uint32_t>(
            static_cast<std::int32_t>(static_cast<std::int16_t>(std::llround(res))));
        break;
    }
    case 0x0A: {
        const double x = static_cast<double>(static_cast<std::int16_t>(regs.r[0]));
        const double y = static_cast<double>(static_cast<std::int16_t>(regs.r[1]));
        double angle = std::atan2(y, x);
        if (angle < 0.0) {
            angle += Tau;
        }
        const auto scaled = static_cast<std::uint32_t>(std::llround((angle / Tau) * 65536.0));
        regs.r[0] = scaled & 0xFFFFu;
        break;
    }
    case 0x0B: {
        std::uint32_t src = regs.r[0];
        std::uint32_t dst = regs.r[1];
        const std::uint32_t cnt = regs.r[2];
        const std::uint32_t count = cnt & 0x1FFFFFu;
        const bool fixed = (cnt & (1u << 24)) != 0;
        const bool is_32 = (cnt & (1u << 26)) != 0;
        if (is_32) {
            src &= ~3u;
            dst &= ~3u;
            const std::uint32_t fixed_val = bus.ReadU32(src);
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint32_t v = fixed ? fixed_val : bus.ReadU32(src);
                bus.WriteU32(dst, v);
                if (!fixed) {
                    src += 4u;
                }
                dst += 4u;
            }
        } else {
            src &= ~1u;
            dst &= ~1u;
            const std::uint16_t fixed_val = bus.ReadU16(src);
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint16_t v = fixed ? fixed_val : bus.ReadU16(src);
                bus.WriteU16(dst, v);
                if (!fixed) {
                    src += 2u;
                }
                dst += 2u;
            }
        }
        break;
    }
    case 0x0C: {
        std::uint32_t src = regs.r[0] & ~3u;
        std::uint32_t dst = regs.r[1] & ~3u;
        const std::uint32_t cnt = regs.r[2];
        const std::uint32_t words = ((cnt & 0x1FFFFFu) + 7u) & ~7u;
        const bool fixed = (cnt & (1u << 24)) != 0;
        const std::uint32_t fixed_val = bus.ReadU32(src);
        for (std::uint32_t i = 0; i < words; ++i) {
            const std::uint32_t v = fixed ? fixed_val : bus.ReadU32(src);
            bus.WriteU32(dst, v);
            if (!fixed) {
                src += 4u;
            }
            dst += 4u;
        }
        break;
    }
    case 0x0D:
        regs.r[0] = 0xBAAE187Fu;
        break;
    case 0x0E: {
        std::uint32_t src = regs.r[0];
        std::uint32_t dst = regs.r[1];
        const std::uint32_t count = regs.r[2];
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto cx = static_cast<std::int32_t>(bus.ReadU32(src));
            const auto cy = static_cast<std::int32_t>(bus.ReadU32(src + 4u));
            const auto dx = static_cast<std::int32_t>(static_cast<std::int16_t>(bus.ReadU16(src + 8u)));
            const auto dy = static_cast<std::int32_t>(static_cast<std::int16_t>(bus.ReadU16(src + 10u)));
            const auto sx = static_cast<std::int32_t>(static_cast<std::int16_t>(bus.ReadU16(src + 12u)));
            const auto sy = static_cast<std::int32_t>(static_cast<std::int16_t>(bus.ReadU16(src + 14u)));
            const double angle = static_cast<double>(bus.ReadU16(src + 16u)) * Tau / 65536.0;
            src += 20u;

            const double sin_a = std::sin(angle);
            const double cos_a = std::cos(angle);
            const auto pa = static_cast<std::int16_t>(std::llround(static_cast<double>(sx) * cos_a));
            const auto pb = static_cast<std::int16_t>(std::llround(-static_cast<double>(sx) * sin_a));
            const auto pc = static_cast<std::int16_t>(std::llround(static_cast<double>(sy) * sin_a));
            const auto pd = static_cast<std::int16_t>(std::llround(static_cast<double>(sy) * cos_a));
            const std::int32_t rx = cx - (static_cast<std::int32_t>(pa) * dx + static_cast<std::int32_t>(pb) * dy);
            const std::int32_t ry = cy - (static_cast<std::int32_t>(pc) * dx + static_cast<std::int32_t>(pd) * dy);

            bus.WriteU16(dst, static_cast<std::uint16_t>(pa));
            bus.WriteU16(dst + 2u, static_cast<std::uint16_t>(pb));
            bus.WriteU16(dst + 4u, static_cast<std::uint16_t>(pc));
            bus.WriteU16(dst + 6u, static_cast<std::uint16_t>(pd));
            bus.WriteU32(dst + 8u, static_cast<std::uint32_t>(rx));
            bus.WriteU32(dst + 12u, static_cast<std::uint32_t>(ry));
            dst += 16u;
        }
        break;
    }
    case 0x0F: {
        std::uint32_t src = regs.r[0];
        std::uint32_t dst = regs.r[1];
        const std::uint32_t count = regs.r[2];
        const std::uint32_t stride = regs.r[3];
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto sx = static_cast<std::int32_t>(static_cast<std::int16_t>(bus.ReadU16(src)));
            const auto sy = static_cast<std::int32_t>(static_cast<std::int16_t>(bus.ReadU16(src + 2u)));
            const double angle = static_cast<double>(bus.ReadU16(src + 4u)) * Tau / 65536.0;
            src += 8u;

            const double sin_a = std::sin(angle);
            const double cos_a = std::cos(angle);
            const auto pa = static_cast<std::int16_t>(std::llround(static_cast<double>(sx) * cos_a));
            const auto pb = static_cast<std::int16_t>(std::llround(-static_cast<double>(sx) * sin_a));
            const auto pc = static_cast<std::int16_t>(std::llround(static_cast<double>(sy) * sin_a));
            const auto pd = static_cast<std::int16_t>(std::llround(static_cast<double>(sy) * cos_a));

            bus.WriteU16(dst, static_cast<std::uint16_t>(pa));
            dst += stride;
            bus.WriteU16(dst, static_cast<std::uint16_t>(pb));
            dst += stride;
            bus.WriteU16(dst, static_cast<std::uint16_t>(pc));
            dst += stride;
            bus.WriteU16(dst, static_cast<std::uint16_t>(pd));
            dst += stride;
        }
        break;
    }
    case 0x10:
        BitUnPack(regs.r[0], regs.r[1], regs.r[2], bus);
        break;
    case 0x11:
    case 0x12:
        Lz77Decomp(regs.r[0], regs.r[1], comment == 0x12, bus);
        break;
    case 0x14:
    case 0x15:
        RlDecomp(regs.r[0], regs.r[1], comment == 0x15, bus);
        break;
    case 0x19: {
        const std::uint16_t level = (regs.r[0] != 0) ? 0x200u : 0u;
        const std::uint16_t cur = bus.ReadU16(0x04000088u);
        bus.WriteU16(0x04000088u, static_cast<std::uint16_t>((cur & ~0x3FFu) | level));
        break;
    }
    case 0x1F: {
        const std::uint32_t wave = regs.r[0];
        const auto mk = static_cast<std::int32_t>(regs.r[1]);
        const std::uint32_t fp = regs.r[2];
        const std::uint32_t base = bus.ReadU32(wave + 4u);
        const double exp = (static_cast<double>(mk - 69) + static_cast<double>(fp) / 256.0) / 12.0;
        regs.r[0] = static_cast<std::uint32_t>(static_cast<double>(base) * std::pow(2.0, exp));
        break;
    }
    default:
        break;
    }
}

} // namespace Emulation::Gba
