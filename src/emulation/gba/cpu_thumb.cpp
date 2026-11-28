// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "emulation/gba/cpu.h"
#include "emulation/gba/bios.h"

#include <array>
#include <bit>

namespace Emulation::Gba {
namespace {

std::uint32_t ExecThumbNop(std::uint16_t /*op*/, CpuRegs& /*regs*/, Bus& /*bus*/) {
    return 1;
}

// Format 1: Move shifted register
std::uint32_t ExecThumbShift(std::uint16_t op, CpuRegs& regs, Bus& /*bus*/) {
    const std::uint16_t sub_op = (op >> 11) & 0x3u;
    const std::size_t rs = (op >> 3) & 0x7u;
    const std::size_t rd = op & 0x7u;
    const std::uint32_t offset = (op >> 6) & 0x1Fu;
    const std::uint32_t val = regs.r[rs];
    bool carry = (regs.cpsr & FlagC) != 0;
    std::uint32_t res = 0;
    switch (sub_op) {
    case 0:
        if (offset == 0) {
            res = val;
        } else {
            carry = ((val >> (32u - offset)) & 1u) != 0;
            res = val << offset;
        }
        break;
    case 1:
        if (offset == 0) {
            carry = (val & 0x80000000u) != 0;
            res = 0;
        } else {
            carry = ((val >> (offset - 1u)) & 1u) != 0;
            res = val >> offset;
        }
        break;
    default:
        if (offset == 0) {
            carry = (val & 0x80000000u) != 0;
            res = carry ? 0xFFFFFFFFu : 0u;
        } else {
            carry = ((val >> (offset - 1u)) & 1u) != 0;
            res = static_cast<std::uint32_t>(static_cast<std::int32_t>(val) >> offset);
        }
        break;
    }
    regs.r[rd] = res;
    UpdateNZC(regs, res, carry);
    return 1;
}

// Format 2: Add/subtract
std::uint32_t ExecThumbAddSub(std::uint16_t op, CpuRegs& regs, Bus& /*bus*/) {
    const bool imm_flag = (op & (1u << 10)) != 0;
    const bool is_sub = (op & (1u << 9)) != 0;
    const std::uint32_t rn_or_imm = (op >> 6) & 0x7u;
    const std::size_t rs = (op >> 3) & 0x7u;
    const std::size_t rd = op & 0x7u;
    const std::uint32_t rhs = imm_flag ? rn_or_imm : regs.r[rn_or_imm];
    const std::uint32_t lhs = regs.r[rs];
    regs.r[rd] = is_sub ? SubWithFlags(regs, lhs, rhs, true) : AddWithFlags(regs, lhs, rhs, true);
    return 1;
}

// Format 3: Move/Compare/Add/Subtract immediate
std::uint32_t ExecThumbImm(std::uint16_t op, CpuRegs& regs, Bus& /*bus*/) {
    const std::uint16_t sub_op = (op >> 11) & 0x3u;
    const std::size_t rd = (op >> 8) & 0x7u;
    const std::uint32_t imm = op & 0xFFu;
    switch (sub_op) {
    case 0:
        regs.r[rd] = imm;
        regs.SetNZ(imm);
        break;
    case 1:
        (void)SubWithFlags(regs, regs.r[rd], imm, true);
        break;
    case 2:
        regs.r[rd] = AddWithFlags(regs, regs.r[rd], imm, true);
        break;
    default:
        regs.r[rd] = SubWithFlags(regs, regs.r[rd], imm, true);
        break;
    }
    return 1;
}

// Format 4: ALU operations
std::uint32_t ExecThumbAlu(std::uint16_t op, CpuRegs& regs, Bus& /*bus*/) {
    const std::uint16_t sub = (op >> 6) & 0xFu;
    const std::size_t rs = (op >> 3) & 0x7u;
    const std::size_t rd = op & 0x7u;
    const std::uint32_t a = regs.r[rd];
    const std::uint32_t b = regs.r[rs];

    switch (sub) {
    case 0x0:
        regs.r[rd] = a & b;
        regs.SetNZ(regs.r[rd]);
        break;
    case 0x1:
        regs.r[rd] = a ^ b;
        regs.SetNZ(regs.r[rd]);
        break;
    case 0x2: {
        const std::uint32_t amt = b & 0xFFu;
        std::uint32_t r = 0;
        bool c = (regs.cpsr & FlagC) != 0;
        if (amt == 0) {
            r = a;
        } else if (amt < 32u) {
            r = a << amt;
            c = ((a >> (32u - amt)) & 1u) != 0;
        } else if (amt == 32u) {
            r = 0;
            c = (a & 1u) != 0;
        } else {
            r = 0;
            c = false;
        }
        regs.r[rd] = r;
        UpdateNZC(regs, r, c);
        break;
    }
    case 0x3: {
        const std::uint32_t amt = b & 0xFFu;
        std::uint32_t r = 0;
        bool c = (regs.cpsr & FlagC) != 0;
        if (amt == 0) {
            r = a;
        } else if (amt < 32u) {
            r = a >> amt;
            c = ((a >> (amt - 1u)) & 1u) != 0;
        } else if (amt == 32u) {
            r = 0;
            c = (a & 0x80000000u) != 0;
        } else {
            r = 0;
            c = false;
        }
        regs.r[rd] = r;
        UpdateNZC(regs, r, c);
        break;
    }
    case 0x4: {
        const std::uint32_t amt = b & 0xFFu;
        std::uint32_t r = 0;
        bool c = (regs.cpsr & FlagC) != 0;
        if (amt == 0) {
            r = a;
        } else if (amt < 32u) {
            r = static_cast<std::uint32_t>(static_cast<std::int32_t>(a) >> amt);
            c = ((a >> (amt - 1u)) & 1u) != 0;
        } else {
            const bool neg = (a & 0x80000000u) != 0;
            r = neg ? 0xFFFFFFFFu : 0u;
            c = neg;
        }
        regs.r[rd] = r;
        UpdateNZC(regs, r, c);
        break;
    }
    case 0x5:
        regs.r[rd] = AdcWithFlags(regs, a, b, true);
        break;
    case 0x6:
        regs.r[rd] = SbcWithFlags(regs, a, b, true);
        break;
    case 0x7: {
        const std::uint32_t amt = b & 0xFFu;
        std::uint32_t r = a;
        bool c = (regs.cpsr & FlagC) != 0;
        if (amt != 0) {
            const std::uint32_t eff = amt & 31u;
            if (eff == 0) {
                r = a;
                c = (a & 0x80000000u) != 0;
            } else {
                r = std::rotr(a, static_cast<int>(eff));
                c = (r & 0x80000000u) != 0;
            }
        }
        regs.r[rd] = r;
        UpdateNZC(regs, r, c);
        break;
    }
    case 0x8:
        regs.SetNZ(a & b);
        break;
    case 0x9:
        regs.r[rd] = SubWithFlags(regs, 0, b, true);
        break;
    case 0xA:
        (void)SubWithFlags(regs, a, b, true);
        break;
    case 0xB:
        (void)AddWithFlags(regs, a, b, true);
        break;
    case 0xC:
        regs.r[rd] = a | b;
        regs.SetNZ(regs.r[rd]);
        break;
    case 0xD:
        regs.r[rd] = a * b;
        regs.SetNZ(regs.r[rd]);
        break;
    case 0xE:
        regs.r[rd] = a & ~b;
        regs.SetNZ(regs.r[rd]);
        break;
    default:
        regs.r[rd] = ~b;
        regs.SetNZ(regs.r[rd]);
        break;
    }
    return 1;
}

// Format 5: Hi register operations / branch exchange
std::uint32_t ExecThumbHiReg(std::uint16_t op, CpuRegs& regs, Bus& /*bus*/) {
    const std::uint32_t pc_plus_4 = regs.r[15] + 2u;
    const std::uint16_t sub = (op >> 8) & 0x3u;
    const std::size_t h1 = ((op >> 7) & 1u) << 3;
    const std::size_t h2 = ((op >> 6) & 1u) << 3;
    const std::size_t rs = h2 | ((op >> 3) & 0x7u);
    const std::size_t rd = h1 | (op & 0x7u);
    const std::uint32_t rhs = (rs == 15) ? pc_plus_4 : regs.r[rs];

    switch (sub) {
    case 0: {
        const std::uint32_t lhs = (rd == 15) ? pc_plus_4 : regs.r[rd];
        const std::uint32_t res = lhs + rhs;
        if (rd == 15) {
            regs.r[15] = res & ~1u;
            return 3;
        }
        regs.r[rd] = res;
        return 1;
    }
    case 1: {
        const std::uint32_t lhs = (rd == 15) ? pc_plus_4 : regs.r[rd];
        (void)SubWithFlags(regs, lhs, rhs, true);
        return 1;
    }
    case 2:
        if (rd == 15) {
            regs.r[15] = rhs & ~1u;
            return 3;
        }
        regs.r[rd] = rhs;
        return 1;
    default:
        if ((rhs & 1u) != 0) {
            regs.cpsr |= FlagT;
            regs.r[15] = rhs & ~1u;
        } else {
            regs.cpsr &= ~FlagT;
            regs.r[15] = rhs & ~3u;
        }
        return 3;
    }
}

// Format 6: PC-relative load
std::uint32_t ExecThumbPcRelLoad(std::uint16_t op, CpuRegs& regs, Bus& bus) {
    const std::uint32_t pc_plus_4 = regs.r[15] + 2u;
    const std::size_t rd = (op >> 8) & 0x7u;
    const std::uint32_t imm = static_cast<std::uint32_t>(op & 0xFFu) << 2;
    const std::uint32_t addr = (pc_plus_4 & ~3u) + imm;
    regs.r[rd] = bus.ReadU32(addr);
    return 3;
}

// Format 7: Load/store with register offset
std::uint32_t ExecThumbRegOffset(std::uint16_t op, CpuRegs& regs, Bus& bus) {
    const bool load = (op & (1u << 11)) != 0;
    const bool byte = (op & (1u << 10)) != 0;
    const std::size_t ro = (op >> 6) & 0x7u;
    const std::size_t rb = (op >> 3) & 0x7u;
    const std::size_t rd = op & 0x7u;
    const std::uint32_t addr = regs.r[rb] + regs.r[ro];
    if (load) {
        regs.r[rd] = byte ? static_cast<std::uint32_t>(bus.ReadU8(addr)) : CpuReadU32Rotated(bus, addr);
        return 3;
    }
    if (byte) {
        bus.WriteU8(addr, static_cast<std::uint8_t>(regs.r[rd]));
    } else {
        bus.WriteU32(addr, regs.r[rd]);
    }
    return 2;
}

// Format 8: Load/store sign-extended byte/halfword
std::uint32_t ExecThumbSignExt(std::uint16_t op, CpuRegs& regs, Bus& bus) {
    const bool h = (op & (1u << 11)) != 0;
    const bool s = (op & (1u << 10)) != 0;
    const std::size_t ro = (op >> 6) & 0x7u;
    const std::size_t rb = (op >> 3) & 0x7u;
    const std::size_t rd = op & 0x7u;
    const std::uint32_t addr = regs.r[rb] + regs.r[ro];

    if (!s && !h) {
        bus.WriteU16(addr, static_cast<std::uint16_t>(regs.r[rd]));
        return 2;
    }
    if (!s && h) {
        regs.r[rd] = CpuReadU16Rotated(bus, addr);
        return 3;
    }
    if (s && !h) {
        regs.r[rd] =
            static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(bus.ReadU8(addr))));
        return 3;
    }
    regs.r[rd] = CpuReadS16Arm(bus, addr);
    return 3;
}

// Format 9: Load/store with immediate offset
std::uint32_t ExecThumbImmOffset(std::uint16_t op, CpuRegs& regs, Bus& bus) {
    const bool byte = (op & (1u << 12)) != 0;
    const bool load = (op & (1u << 11)) != 0;
    const std::uint32_t off5 = (op >> 6) & 0x1Fu;
    const std::size_t rb = (op >> 3) & 0x7u;
    const std::size_t rd = op & 0x7u;
    const std::uint32_t addr = regs.r[rb] + (byte ? off5 : (off5 << 2));
    if (load) {
        regs.r[rd] = byte ? static_cast<std::uint32_t>(bus.ReadU8(addr)) : CpuReadU32Rotated(bus, addr);
        return 3;
    }
    if (byte) {
        bus.WriteU8(addr, static_cast<std::uint8_t>(regs.r[rd]));
    } else {
        bus.WriteU32(addr, regs.r[rd]);
    }
    return 2;
}

// Format 10: Load/store halfword
std::uint32_t ExecThumbHalfword(std::uint16_t op, CpuRegs& regs, Bus& bus) {
    const std::uint32_t off = static_cast<std::uint32_t>((op >> 6) & 0x1Fu) << 1;
    const std::size_t rb = (op >> 3) & 0x7u;
    const std::size_t rd = op & 0x7u;
    const std::uint32_t addr = regs.r[rb] + off;
    if ((op & (1u << 11)) != 0) {
        regs.r[rd] = CpuReadU16Rotated(bus, addr);
        return 3;
    }
    bus.WriteU16(addr, static_cast<std::uint16_t>(regs.r[rd]));
    return 2;
}

// Format 11: SP-relative load/store
std::uint32_t ExecThumbSpRel(std::uint16_t op, CpuRegs& regs, Bus& bus) {
    const std::size_t rd = (op >> 8) & 0x7u;
    const std::uint32_t imm = static_cast<std::uint32_t>(op & 0xFFu) << 2;
    const std::uint32_t addr = regs.r[13] + imm;
    if ((op & (1u << 11)) != 0) {
        regs.r[rd] = CpuReadU32Rotated(bus, addr);
        return 3;
    }
    bus.WriteU32(addr, regs.r[rd]);
    return 2;
}

// Format 12: Load address
std::uint32_t ExecThumbLoadAddr(std::uint16_t op, CpuRegs& regs, Bus& /*bus*/) {
    const std::uint32_t pc_plus_4 = regs.r[15] + 2u;
    const bool sp_src = (op & (1u << 11)) != 0;
    const std::size_t rd = (op >> 8) & 0x7u;
    const std::uint32_t imm = static_cast<std::uint32_t>(op & 0xFFu) << 2;
    const std::uint32_t base = sp_src ? regs.r[13] : (pc_plus_4 & ~3u);
    regs.r[rd] = base + imm;
    return 1;
}

// Format 13: Add offset to SP
std::uint32_t ExecThumbAddSp(std::uint16_t op, CpuRegs& regs, Bus& /*bus*/) {
    const std::uint32_t imm = static_cast<std::uint32_t>(op & 0x7Fu) << 2;
    if ((op & (1u << 7)) != 0) {
        regs.r[13] -= imm;
    } else {
        regs.r[13] += imm;
    }
    return 1;
}

// Format 14: Push/Pop registers
std::uint32_t ExecThumbPushPop(std::uint16_t op, CpuRegs& regs, Bus& bus) {
    const bool load = (op & (1u << 11)) != 0;
    const bool pclr = (op & (1u << 8)) != 0;
    const std::uint32_t rlist = op & 0xFFu;
    std::uint32_t sp = regs.r[13];
    std::uint32_t cycles = 1;
    if (load) {
        for (std::size_t r = 0; r < 8; ++r) {
            if ((rlist & (1u << r)) != 0) {
                regs.r[r] = bus.ReadU32(sp);
                sp += 4u;
                ++cycles;
            }
        }
        if (pclr) {
            regs.r[15] = bus.ReadU32(sp) & ~1u;
            sp += 4u;
            cycles += 3u;
        }
    } else {
        if (pclr) {
            sp -= 4u;
            bus.WriteU32(sp, regs.r[14]);
            ++cycles;
        }
        for (int r = 7; r >= 0; --r) {
            if ((rlist & (1u << r)) != 0) {
                sp -= 4u;
                bus.WriteU32(sp, regs.r[static_cast<std::size_t>(r)]);
                ++cycles;
            }
        }
    }
    regs.r[13] = sp;
    return cycles;
}

// Format 15: Multiple load/store
std::uint32_t ExecThumbLdmStm(std::uint16_t op, CpuRegs& regs, Bus& bus) {
    const std::uint32_t pc_plus_4 = regs.r[15] + 2u;
    const bool load = (op & (1u << 11)) != 0;
    const std::size_t rb = (op >> 8) & 0x7u;
    const std::uint32_t rlist = op & 0xFFu;
    std::uint32_t addr = regs.r[rb];
    std::uint32_t cycles = 1;
    if (rlist == 0) {
        if (load) {
            regs.r[15] = bus.ReadU32(addr) & ~1u;
        } else {
            bus.WriteU32(addr, pc_plus_4 + 2u);
        }
        regs.r[rb] = addr + 0x40u;
        return 4;
    }
    if (load) {
        for (std::size_t r = 0; r < 8; ++r) {
            if ((rlist & (1u << r)) != 0) {
                regs.r[r] = bus.ReadU32(addr);
                addr += 4u;
                ++cycles;
            }
        }
        if ((rlist & (1u << rb)) == 0) {
            regs.r[rb] = addr;
        }
    } else {
        const std::uint32_t final_base =
            addr + static_cast<std::uint32_t>(std::popcount(rlist)) * 4u;
        bool first = true;
        for (std::size_t r = 0; r < 8; ++r) {
            if ((rlist & (1u << r)) != 0) {
                const std::uint32_t val = (r == rb && !first) ? final_base : regs.r[r];
                bus.WriteU32(addr, val);
                addr += 4u;
                ++cycles;
                first = false;
            }
        }
        regs.r[rb] = final_base;
    }
    return cycles;
}

// Format 16: Conditional branch
std::uint32_t ExecThumbCondBranch(std::uint16_t op, CpuRegs& regs, Bus& /*bus*/) {
    const std::uint32_t cond = (op >> 8) & 0xFu;
    if (regs.ConditionPassed(cond)) {
        const std::uint32_t pc_plus_4 = regs.r[15] + 2u;
        const std::int32_t off =
            static_cast<std::int32_t>(static_cast<std::int8_t>(op & 0xFFu)) * 2;
        regs.r[15] =
            static_cast<std::uint32_t>(static_cast<std::int64_t>(pc_plus_4) + off) & ~1u;
        return 3;
    }
    return 1;
}

// Format 17: Software Interrupt
std::uint32_t ExecThumbSwi(std::uint16_t op, CpuRegs& regs, Bus& bus) {
    const auto comment = static_cast<std::uint8_t>(op & 0xFFu);
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

// Format 18: Unconditional branch
std::uint32_t ExecThumbUncondBranch(std::uint16_t op, CpuRegs& regs, Bus& /*bus*/) {
    const std::uint32_t pc_plus_4 = regs.r[15] + 2u;
    const std::int32_t raw = static_cast<std::int32_t>(op & 0x7FFu);
    const std::int32_t off = (raw << 21) >> 20;
    regs.r[15] = static_cast<std::uint32_t>(static_cast<std::int64_t>(pc_plus_4) + off) & ~1u;
    return 3;
}

// Format 19: Long branch with link
std::uint32_t ExecThumbBl(std::uint16_t op, CpuRegs& regs, Bus& /*bus*/) {
    const std::uint32_t pc_plus_4 = regs.r[15] + 2u;
    const std::uint32_t imm = op & 0x7FFu;
    if ((op & (1u << 11)) == 0) {
        const auto hi = static_cast<std::uint32_t>(static_cast<std::int32_t>(imm << 21) >> 9);
        regs.r[14] = pc_plus_4 + hi;
        return 1;
    }
    const std::uint32_t target = (regs.r[14] + (imm << 1)) & ~1u;
    regs.r[14] = regs.r[15] | 1u;
    regs.r[15] = target;
    return 3;
}

using ThumbHandler = std::uint32_t (*)(std::uint16_t op, CpuRegs& regs, Bus& bus);

consteval ThumbHandler SelectThumbHandler(std::size_t idx) {
    const std::uint16_t op = static_cast<std::uint16_t>(idx << 6);
    switch ((op >> 13) & 0x7u) {
    case 0:
        return (((op >> 11) & 0x3u) == 3u) ? &ExecThumbAddSub : &ExecThumbShift;
    case 1:
        return &ExecThumbImm;
    case 2:
        if ((op & (1u << 12)) == 0) {
            if ((op & (1u << 11)) == 0) {
                return ((op & (1u << 10)) == 0) ? &ExecThumbAlu : &ExecThumbHiReg;
            }
            return &ExecThumbPcRelLoad;
        }
        return ((op & (1u << 9)) == 0) ? &ExecThumbRegOffset : &ExecThumbSignExt;
    case 3:
        return &ExecThumbImmOffset;
    case 4:
        return ((op & (1u << 12)) == 0) ? &ExecThumbHalfword : &ExecThumbSpRel;
    case 5:
        if ((op & (1u << 12)) == 0) {
            return &ExecThumbLoadAddr;
        }
        if ((op & 0x0F00u) == 0) {
            return &ExecThumbAddSp;
        }
        if ((op & 0x0600u) == 0x0400u) {
            return &ExecThumbPushPop;
        }
        return &ExecThumbNop;
    case 6:
        if ((op & (1u << 12)) == 0) {
            return &ExecThumbLdmStm;
        }
        {
            const std::uint16_t cond = (op >> 8) & 0xFu;
            if (cond == 0xFu) {
                return &ExecThumbSwi;
            }
            if (cond < 0xEu) {
                return &ExecThumbCondBranch;
            }
            return &ExecThumbNop;
        }
    default:
        return ((op & (1u << 12)) == 0) ? &ExecThumbUncondBranch : &ExecThumbBl;
    }
}

consteval std::array<ThumbHandler, 1024> BuildThumbLut() {
    std::array<ThumbHandler, 1024> table{};
    for (std::size_t i = 0; i < 1024; ++i) {
        table[i] = SelectThumbHandler(i);
    }
    return table;
}

constexpr std::array<ThumbHandler, 1024> ThumbLut = BuildThumbLut();

} // namespace

std::uint32_t ExecThumb(std::uint16_t op, CpuRegs& regs, Bus& bus) {
    return ThumbLut[op >> 6](op, regs, bus);
}

} // namespace Emulation::Gba
