// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "emulation/gba/cpu.h"
#include "emulation/gba/bios.h"

#include <array>
#include <bit>
#include <utility>

namespace Emulation::Gba {
namespace {

inline std::uint32_t RegVal(const CpuRegs& regs, std::size_t reg, std::uint32_t pc_plus_8) {
    return (reg == 15) ? pc_plus_8 : regs.r[reg];
}

std::pair<std::uint32_t, bool> BarrelShift(std::uint32_t val, std::uint32_t shift_type,
                                           std::uint32_t amount, bool imm_form, bool old_carry) {
    switch (shift_type) {
    case 0: // LSL
        if (amount == 0) {
            return {val, old_carry};
        }
        if (amount < 32u) {
            return {val << amount, ((val >> (32u - amount)) & 1u) != 0};
        }
        if (amount == 32u) {
            return {0, (val & 1u) != 0};
        }
        return {0, false};
    case 1: // LSR
        if (imm_form && amount == 0) {
            return {0, (val & 0x80000000u) != 0};
        }
        if (amount == 0) {
            return {val, old_carry};
        }
        if (amount < 32u) {
            return {val >> amount, ((val >> (amount - 1u)) & 1u) != 0};
        }
        if (amount == 32u) {
            return {0, (val & 0x80000000u) != 0};
        }
        return {0, false};
    case 2: // ASR
        if (imm_form && amount == 0) {
            const bool neg = (val & 0x80000000u) != 0;
            return {neg ? 0xFFFFFFFFu : 0u, neg};
        }
        if (amount == 0) {
            return {val, old_carry};
        }
        if (amount < 32u) {
            return {static_cast<std::uint32_t>(static_cast<std::int32_t>(val) >> amount),
                    ((val >> (amount - 1u)) & 1u) != 0};
        } else {
            const bool neg = (val & 0x80000000u) != 0;
            return {neg ? 0xFFFFFFFFu : 0u, neg};
        }
    default: // ROR / RRX
        if (imm_form && amount == 0) {
            const bool c = (val & 1u) != 0;
            const std::uint32_t res = (val >> 1) | (old_carry ? 0x80000000u : 0u);
            return {res, c};
        }
        if (amount == 0) {
            return {val, old_carry};
        }
        {
            const std::uint32_t eff = amount & 31u;
            if (eff == 0) {
                return {val, (val & 0x80000000u) != 0};
            }
            const std::uint32_t res = std::rotr(val, static_cast<int>(eff));
            return {res, (res & 0x80000000u) != 0};
        }
    }
}

std::uint32_t ExecDataProcessing(std::uint32_t op, CpuRegs& regs, Bus& /*bus*/) {
    const std::uint32_t pc_plus_8 = regs.r[15] + 4u;
    const std::uint32_t opcode = (op >> 21) & 0xFu;
    const bool s_bit = (op & (1u << 20)) != 0;
    const std::size_t rn_idx = (op >> 16) & 0xFu;
    const std::size_t rd_idx = (op >> 12) & 0xFu;

    const bool old_carry = (regs.cpsr & FlagC) != 0;
    std::uint32_t reg_shift_extra = 0;

    std::uint32_t op2 = 0;
    bool shifter_carry = old_carry;
    if ((op & (1u << 25)) != 0) {
        const std::uint32_t imm = op & 0xFFu;
        const std::uint32_t rot = ((op >> 8) & 0xFu) * 2u;
        if (rot == 0) {
            op2 = imm;
            shifter_carry = old_carry;
        } else {
            op2 = std::rotr(imm, static_cast<int>(rot));
            shifter_carry = (op2 & 0x80000000u) != 0;
        }
    } else {
        const std::size_t rm_idx = op & 0xFu;
        const std::uint32_t shift_type = (op >> 5) & 0x3u;
        const bool by_reg = (op & (1u << 4)) != 0;
        const std::uint32_t rm_val =
            (by_reg && rm_idx == 15) ? (pc_plus_8 + 4u) : RegVal(regs, rm_idx, pc_plus_8);
        std::uint32_t amount = 0;
        if (by_reg) {
            reg_shift_extra = 1;
            const std::size_t rs_idx = (op >> 8) & 0xFu;
            amount = regs.r[rs_idx] & 0xFFu;
        } else {
            amount = (op >> 7) & 0x1Fu;
        }
        std::tie(op2, shifter_carry) = BarrelShift(rm_val, shift_type, amount, !by_reg, old_carry);
    }

    const bool rn_reg_shift_pc =
        ((op & (1u << 25)) == 0) && ((op & (1u << 4)) != 0) && (rn_idx == 15);
    const std::uint32_t rn =
        rn_reg_shift_pc ? (pc_plus_8 + 4u) : RegVal(regs, rn_idx, pc_plus_8);

    bool write_rd = true;
    std::uint32_t result = 0;
    switch (opcode) {
    case 0x0:
        result = rn & op2;
        if (s_bit) {
            UpdateNZC(regs, result, shifter_carry);
        }
        break;
    case 0x1:
        result = rn ^ op2;
        if (s_bit) {
            UpdateNZC(regs, result, shifter_carry);
        }
        break;
    case 0x2:
        result = SubWithFlags(regs, rn, op2, s_bit);
        break;
    case 0x3:
        result = SubWithFlags(regs, op2, rn, s_bit);
        break;
    case 0x4:
        result = AddWithFlags(regs, rn, op2, s_bit);
        break;
    case 0x5:
        result = AdcWithFlags(regs, rn, op2, s_bit);
        break;
    case 0x6:
        result = SbcWithFlags(regs, rn, op2, s_bit);
        break;
    case 0x7:
        result = SbcWithFlags(regs, op2, rn, s_bit);
        break;
    case 0x8:
        write_rd = false;
        result = rn & op2;
        if (s_bit) {
            UpdateNZC(regs, result, shifter_carry);
        }
        break;
    case 0x9:
        write_rd = false;
        result = rn ^ op2;
        if (s_bit) {
            UpdateNZC(regs, result, shifter_carry);
        }
        break;
    case 0xA:
        write_rd = false;
        result = SubWithFlags(regs, rn, op2, s_bit);
        break;
    case 0xB:
        write_rd = false;
        result = AddWithFlags(regs, rn, op2, s_bit);
        break;
    case 0xC:
        result = rn | op2;
        if (s_bit) {
            UpdateNZC(regs, result, shifter_carry);
        }
        break;
    case 0xD:
        result = op2;
        if (s_bit) {
            UpdateNZC(regs, result, shifter_carry);
        }
        break;
    case 0xE:
        result = rn & ~op2;
        if (s_bit) {
            UpdateNZC(regs, result, shifter_carry);
        }
        break;
    case 0xF:
        result = ~op2;
        if (s_bit) {
            UpdateNZC(regs, result, shifter_carry);
        }
        break;
    default:
        break;
    }

    if (write_rd) {
        if (rd_idx == 15) {
            if (s_bit) {
                const std::uint32_t spsr = regs.Spsr();
                regs.SetCpsr(spsr);
            }
            regs.r[15] = regs.IsThumb() ? (result & ~1u) : (result & ~3u);
            return 3u + reg_shift_extra;
        }
        regs.r[rd_idx] = result;
    } else if (rd_idx == 15 && s_bit) {
        const std::uint32_t spsr = regs.Spsr();
        regs.SetCpsr(spsr);
    }
    return 1u + reg_shift_extra;
}

std::uint32_t ExecMsr(std::uint32_t op, CpuRegs& regs, std::uint32_t pc_plus_8) {
    std::uint32_t val = 0;
    if ((op & (1u << 25)) != 0) {
        const std::uint32_t imm = op & 0xFFu;
        const std::uint32_t rot = ((op >> 8) & 0xFu) * 2u;
        val = std::rotr(imm, static_cast<int>(rot));
    } else {
        val = RegVal(regs, op & 0xFu, pc_plus_8);
    }
    const bool use_spsr = (op & (1u << 22)) != 0;
    std::uint32_t mask = 0;
    if ((op & (1u << 16)) != 0) {
        mask |= 0x000000FFu;
    }
    if ((op & (1u << 17)) != 0) {
        mask |= 0x0000FF00u;
    }
    if ((op & (1u << 18)) != 0) {
        mask |= 0x00FF0000u;
    }
    if ((op & (1u << 19)) != 0) {
        mask |= 0xFF000000u;
    }

    if (use_spsr) {
        const std::uint32_t spsr = regs.Spsr();
        regs.SetSpsr((spsr & ~mask) | (val & mask));
    } else {
        if (regs.Mode() == CpuMode::User) {
            mask &= 0xF0000000u;
        }
        const std::uint32_t next = (regs.cpsr & ~mask) | (val & mask);
        regs.SetCpsr(next);
    }
    return 1;
}

std::uint32_t ExecBxOrDp(std::uint32_t op, CpuRegs& regs, Bus& bus) {
    if ((op & 0x0FFFFFF0u) == 0x012FFF10u) {
        const std::uint32_t pc_plus_8 = regs.r[15] + 4u;
        const std::uint32_t target = RegVal(regs, op & 0xFu, pc_plus_8);
        if ((target & 1u) != 0) {
            regs.cpsr |= FlagT;
            regs.r[15] = target & ~1u;
        } else {
            regs.cpsr &= ~FlagT;
            regs.r[15] = target & ~3u;
        }
        return 3;
    }
    return ExecDataProcessing(op, regs, bus);
}

std::uint32_t ExecSwi(std::uint32_t op, CpuRegs& regs, Bus& bus) {
    const auto comment = static_cast<std::uint8_t>((op >> 16) & 0xFFu);
    if (bus.bios.HasRealBios()) {
        const std::uint32_t old_cpsr = regs.cpsr;
        const std::uint32_t ret = regs.r[15];
        regs.SwitchMode(CpuMode::Svc);
        regs.spsr_svc = old_cpsr;
        regs.cpsr = (regs.cpsr & ~FlagT) | FlagI;
        regs.r[14] = ret;
        regs.r[15] = 0x00000008u;
    } else {
        HleSwi(comment, regs, bus);
    }
    return 3;
}

std::uint32_t ExecBranch(std::uint32_t op, CpuRegs& regs, Bus& /*bus*/) {
    const std::uint32_t pc_plus_8 = regs.r[15] + 4u;
    if ((op & (1u << 24)) != 0) {
        regs.r[14] = regs.r[15];
    }
    const std::uint32_t imm24 = op & 0x00FFFFFFu;
    const auto offset = static_cast<std::uint32_t>(static_cast<std::int32_t>(imm24 << 8) >> 6);
    regs.r[15] = (pc_plus_8 + offset) & ~3u;
    return 3;
}

std::uint32_t ExecBlockTransfer(std::uint32_t op, CpuRegs& regs, Bus& bus) {
    const std::uint32_t pc_plus_8 = regs.r[15] + 4u;
    const bool pre = (op & (1u << 24)) != 0;
    const bool up = (op & (1u << 23)) != 0;
    const bool s_bit = (op & (1u << 22)) != 0;
    const bool writeback = (op & (1u << 21)) != 0;
    const bool load = (op & (1u << 20)) != 0;
    const std::size_t rn = (op >> 16) & 0xFu;
    const std::uint32_t rlist = op & 0xFFFFu;

    const std::uint32_t base = RegVal(regs, rn, pc_plus_8);
    const std::uint32_t count = static_cast<std::uint32_t>(std::popcount(rlist));
    const std::uint32_t total_bytes = count * 4u;

    std::uint32_t addr = 0;
    std::uint32_t final_base = 0;
    if (count == 0) {
        if (up) {
            addr = pre ? (base + 4u) : base;
            final_base = base + 0x40u;
        } else {
            addr = pre ? (base - 0x40u) : (base - 0x3Cu);
            final_base = base - 0x40u;
        }
    } else if (up) {
        addr = pre ? (base + 4u) : base;
        final_base = base + total_bytes;
    } else {
        final_base = base - total_bytes;
        addr = pre ? final_base : (final_base + 4u);
    }

    const bool user_bank = s_bit && (!load || ((rlist & (1u << 15)) == 0));
    bool pc_loaded = false;

    if (count == 0) {
        if (load) {
            regs.r[15] = bus.ReadU32(addr) & ~3u;
            pc_loaded = true;
        } else {
            bus.WriteU32(addr, pc_plus_8 + 4u);
        }
        if (writeback) {
            regs.r[rn] = final_base;
        }
    } else if (load) {
        // Perform base writeback in the current mode before loading R15^ can switch modes.
        if (writeback && (rlist & (1u << rn)) == 0) {
            regs.r[rn] = final_base;
        }
        for (std::size_t r = 0; r < 16; ++r) {
            if ((rlist & (1u << r)) != 0) {
                const std::uint32_t v = bus.ReadU32(addr);
                addr += 4u;
                if (user_bank) {
                    regs.WriteUserReg(r, v);
                } else if (r == 15) {
                    if (s_bit) {
                        const std::uint32_t spsr = regs.Spsr();
                        regs.SetCpsr(spsr);
                    }
                    regs.r[15] = regs.IsThumb() ? (v & ~1u) : (v & ~3u);
                    pc_loaded = true;
                } else {
                    regs.r[r] = v;
                }
            }
        }
    } else {
        bool first = true;
        for (std::size_t r = 0; r < 16; ++r) {
            if ((rlist & (1u << r)) != 0) {
                std::uint32_t v = 0;
                if (r == 15) {
                    v = pc_plus_8 + 4u;
                } else if (user_bank) {
                    v = regs.ReadUserReg(r);
                } else if (r == rn && !first && writeback) {
                    v = final_base;
                } else {
                    v = regs.r[r];
                }
                bus.WriteU32(addr, v);
                addr += 4u;
                first = false;
            }
        }
        if (writeback) {
            regs.r[rn] = final_base;
        }
    }

    return count + (pc_loaded ? 4u : 2u);
}

std::uint32_t ExecSingleTransfer(std::uint32_t op, CpuRegs& regs, Bus& bus) {
    const std::uint32_t pc_plus_8 = regs.r[15] + 4u;
    const bool reg_off = (op & (1u << 25)) != 0;
    const bool pre = (op & (1u << 24)) != 0;
    const bool up = (op & (1u << 23)) != 0;
    const bool byte = (op & (1u << 22)) != 0;
    const bool writeback = (op & (1u << 21)) != 0;
    const bool load = (op & (1u << 20)) != 0;
    const std::size_t rn = (op >> 16) & 0xFu;
    const std::size_t rd = (op >> 12) & 0xFu;

    const std::uint32_t base = RegVal(regs, rn, pc_plus_8);
    std::uint32_t offset = 0;
    if (!reg_off) {
        offset = op & 0xFFFu;
    } else {
        const std::size_t rm = op & 0xFu;
        const std::uint32_t shift_type = (op >> 5) & 0x3u;
        const std::uint32_t amount = (op >> 7) & 0x1Fu;
        const std::uint32_t rm_val = RegVal(regs, rm, pc_plus_8);
        offset = BarrelShift(rm_val, shift_type, amount, true, (regs.cpsr & FlagC) != 0).first;
    }

    const std::uint32_t eff_addr = up ? (base + offset) : (base - offset);
    const std::uint32_t addr = pre ? eff_addr : base;

    if (load) {
        const std::uint32_t val =
            byte ? static_cast<std::uint32_t>(bus.ReadU8(addr)) : CpuReadU32Rotated(bus, addr);
        if (!pre || writeback) {
            regs.r[rn] = eff_addr;
        }
        if (rd == 15) {
            regs.r[15] = val & ~3u;
            return 5;
        }
        regs.r[rd] = val;
        return 3;
    }

    const std::uint32_t val = (rd == 15) ? (pc_plus_8 + 4u) : regs.r[rd];
    if (byte) {
        bus.WriteU8(addr, static_cast<std::uint8_t>(val));
    } else {
        bus.WriteU32(addr, val);
    }
    if (!pre || writeback) {
        regs.r[rn] = eff_addr;
    }
    return 2;
}

std::uint32_t ExecMul(std::uint32_t op, CpuRegs& regs, Bus& /*bus*/) {
    const std::uint32_t pc_plus_8 = regs.r[15] + 4u;
    const bool acc = (op & (1u << 21)) != 0;
    const bool s_bit = (op & (1u << 20)) != 0;
    const std::size_t rd = (op >> 16) & 0xFu;
    const std::size_t rn = (op >> 12) & 0xFu;
    const std::size_t rs = (op >> 8) & 0xFu;
    const std::size_t rm = op & 0xFu;

    std::uint32_t res = RegVal(regs, rm, pc_plus_8) * RegVal(regs, rs, pc_plus_8);
    if (acc) {
        res += RegVal(regs, rn, pc_plus_8);
    }
    regs.r[rd] = res;
    if (s_bit) {
        regs.SetNZ(res);
    }
    return 2;
}

std::uint32_t ExecMull(std::uint32_t op, CpuRegs& regs, Bus& /*bus*/) {
    const std::uint32_t pc_plus_8 = regs.r[15] + 4u;
    const bool is_signed = (op & (1u << 22)) != 0;
    const bool acc = (op & (1u << 21)) != 0;
    const bool s_bit = (op & (1u << 20)) != 0;
    const std::size_t rd_hi = (op >> 16) & 0xFu;
    const std::size_t rd_lo = (op >> 12) & 0xFu;
    const std::size_t rs = (op >> 8) & 0xFu;
    const std::size_t rm = op & 0xFu;

    const std::uint32_t m1 = RegVal(regs, rm, pc_plus_8);
    const std::uint32_t m2 = RegVal(regs, rs, pc_plus_8);
    std::uint64_t prod = 0;
    if (is_signed) {
        prod = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(m1)) *
                                          static_cast<std::int64_t>(static_cast<std::int32_t>(m2)));
    } else {
        prod = static_cast<std::uint64_t>(m1) * static_cast<std::uint64_t>(m2);
    }
    if (acc) {
        const std::uint64_t hi = regs.r[rd_hi];
        const std::uint64_t lo = regs.r[rd_lo];
        prod += (hi << 32) | lo;
    }
    const auto lo_out = static_cast<std::uint32_t>(prod);
    const auto hi_out = static_cast<std::uint32_t>(prod >> 32);
    regs.r[rd_lo] = lo_out;
    regs.r[rd_hi] = hi_out;
    if (s_bit) {
        regs.cpsr &= ~(FlagN | FlagZ);
        if ((hi_out & 0x80000000u) != 0) {
            regs.cpsr |= FlagN;
        }
        if (prod == 0) {
            regs.cpsr |= FlagZ;
        }
    }
    return 3;
}

std::uint32_t ExecSwpOrDp(std::uint32_t op, CpuRegs& regs, Bus& bus) {
    if ((op & 0x0FB00FF0u) != 0x01000090u) {
        return ExecDataProcessing(op, regs, bus);
    }
    const std::uint32_t pc_plus_8 = regs.r[15] + 4u;
    const bool byte = (op & (1u << 22)) != 0;
    const std::size_t rn = (op >> 16) & 0xFu;
    const std::size_t rd = (op >> 12) & 0xFu;
    const std::size_t rm = op & 0xFu;
    const std::uint32_t addr = RegVal(regs, rn, pc_plus_8);
    const std::uint32_t src = RegVal(regs, rm, pc_plus_8);
    if (byte) {
        const std::uint32_t old = bus.ReadU8(addr);
        bus.WriteU8(addr, static_cast<std::uint8_t>(src));
        regs.r[rd] = old;
    } else {
        const std::uint32_t old = CpuReadU32Rotated(bus, addr);
        bus.WriteU32(addr, src);
        regs.r[rd] = old;
    }
    return 4;
}

std::uint32_t ExecHalfwordTransfer(std::uint32_t op, CpuRegs& regs, Bus& bus) {
    const std::uint32_t pc_plus_8 = regs.r[15] + 4u;
    const bool pre = (op & (1u << 24)) != 0;
    const bool up = (op & (1u << 23)) != 0;
    const bool imm_off = (op & (1u << 22)) != 0;
    const bool writeback = (op & (1u << 21)) != 0;
    const bool load = (op & (1u << 20)) != 0;
    const std::size_t rn = (op >> 16) & 0xFu;
    const std::size_t rd = (op >> 12) & 0xFu;
    const std::uint32_t sh = (op >> 5) & 0x3u;

    const std::uint32_t base = RegVal(regs, rn, pc_plus_8);
    const std::uint32_t offset =
        imm_off ? (((op >> 4) & 0xF0u) | (op & 0xFu)) : RegVal(regs, op & 0xFu, pc_plus_8);
    const std::uint32_t eff_addr = up ? (base + offset) : (base - offset);
    const std::uint32_t addr = pre ? eff_addr : base;

    if (load) {
        std::uint32_t val = 0;
        switch (sh) {
        case 1:
            val = CpuReadU16Rotated(bus, addr);
            break;
        case 2:
            val = static_cast<std::uint32_t>(
                static_cast<std::int32_t>(static_cast<std::int8_t>(bus.ReadU8(addr))));
            break;
        case 3:
            val = CpuReadS16Arm(bus, addr);
            break;
        default:
            break;
        }
        if (!pre || writeback) {
            regs.r[rn] = eff_addr;
        }
        if (rd == 15) {
            regs.r[15] = val & ~3u;
            return 5;
        }
        regs.r[rd] = val;
        return 3;
    }

    const std::uint32_t val = (rd == 15) ? (pc_plus_8 + 4u) : regs.r[rd];
    bus.WriteU16(addr, static_cast<std::uint16_t>(val));
    if (!pre || writeback) {
        regs.r[rn] = eff_addr;
    }
    return 2;
}

std::uint32_t ExecMrsOrDp(std::uint32_t op, CpuRegs& regs, Bus& bus) {
    if ((op & 0x0FB00000u) == 0x01000000u && (op & 0x000F0000u) == 0x000F0000u &&
        (op & 0x00000FFFu) == 0) {
        const std::size_t rd = (op >> 12) & 0xFu;
        const bool use_spsr = (op & (1u << 22)) != 0;
        regs.r[rd] = use_spsr ? regs.Spsr() : regs.cpsr;
        return 1;
    }
    return ExecDataProcessing(op, regs, bus);
}

std::uint32_t ExecMsrRegOrDp(std::uint32_t op, CpuRegs& regs, Bus& bus) {
    if ((op & 0x0FB0FFF0u) == 0x0120F000u) {
        return ExecMsr(op, regs, regs.r[15] + 4u);
    }
    return ExecDataProcessing(op, regs, bus);
}

std::uint32_t ExecMsrImmOrDp(std::uint32_t op, CpuRegs& regs, Bus& bus) {
    if ((op & 0x0FB0F000u) == 0x0320F000u) {
        return ExecMsr(op, regs, regs.r[15] + 4u);
    }
    return ExecDataProcessing(op, regs, bus);
}

using ArmHandler = std::uint32_t (*)(std::uint32_t op, CpuRegs& regs, Bus& bus);

consteval ArmHandler SelectArmHandler(std::size_t idx) {
    if (idx == 0x121u) {
        return &ExecBxOrDp;
    }
    if ((idx & 0xF00u) == 0xF00u) {
        return &ExecSwi;
    }
    if ((idx & 0xE00u) == 0xA00u) {
        return &ExecBranch;
    }
    if ((idx & 0xE00u) == 0x800u) {
        return &ExecBlockTransfer;
    }
    if ((idx & 0xC00u) == 0x400u) {
        return &ExecSingleTransfer;
    }
    if ((idx & 0xE00u) == 0x000u) {
        if ((idx & 0xFC0u) == 0x000u && (idx & 0xFu) == 0x9u) {
            return &ExecMul;
        }
        if ((idx & 0xF80u) == 0x080u && (idx & 0xFu) == 0x9u) {
            return &ExecMull;
        }
        if ((idx & 0xFB0u) == 0x100u && (idx & 0xFu) == 0x9u) {
            return &ExecSwpOrDp;
        }
        if ((idx & 0x9u) == 0x9u && (idx & 0x6u) != 0u) {
            return &ExecHalfwordTransfer;
        }
    }
    if ((idx & 0xFB0u) == 0x100u && (idx & 0xFu) == 0x0u) {
        return &ExecMrsOrDp;
    }
    if ((idx & 0xFB0u) == 0x120u && (idx & 0xFu) == 0x0u) {
        return &ExecMsrRegOrDp;
    }
    if ((idx & 0xFB0u) == 0x320u) {
        return &ExecMsrImmOrDp;
    }
    return &ExecDataProcessing;
}

consteval std::array<ArmHandler, 4096> BuildArmLut() {
    std::array<ArmHandler, 4096> table{};
    for (std::size_t i = 0; i < 4096; ++i) {
        table[i] = SelectArmHandler(i);
    }
    return table;
}

constexpr std::array<ArmHandler, 4096> ArmLut = BuildArmLut();

} // namespace

std::uint32_t ExecArm(std::uint32_t op, CpuRegs& regs, Bus& bus) {
    const std::uint32_t cond = op >> 28;
    if (cond != 0xEu && !regs.ConditionPassed(cond)) {
        return 1;
    }
    const std::size_t idx = ((op >> 16) & 0xFF0u) | ((op >> 4) & 0xFu);
    return ArmLut[idx](op, regs, bus);
}

} // namespace Emulation::Gba
