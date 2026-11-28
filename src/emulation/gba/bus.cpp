// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "emulation/gba/bus.h"
#include "emulation/gba/types.h"

#include <algorithm>
#include <cstring>

namespace Emulation::Gba {
namespace {

std::size_t VramOffset(std::uint32_t addr) {
    const std::size_t off = static_cast<std::size_t>(addr) & 0x1FFFFu;
    return (off >= 0x18000u) ? (off - 0x8000u) : off;
}

std::uint16_t LoadLe16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t LoadLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void StoreLe16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFFu);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

void StoreLe32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFFu);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

} // namespace

Bus::Bus(Bios bios_in, Cartridge cart_in)
    : bios(std::move(bios_in)), cartridge(std::move(cart_in)), ewram(EwramSize, 0),
      iwram(IwramSize, 0) {}

void Bus::Reset() {
    std::fill(ewram.begin(), ewram.end(), 0);
    std::fill(iwram.begin(), iwram.end(), 0);
    ppu.Reset();
    apu.Reset();
    dma.Reset();
    timers.Reset();
    ie = 0;
    if_flags = 0;
    ime = 0;
    waitcnt = 0;
    postflg = 0;
    keyinput = static_cast<std::uint16_t>(KeyMask);
    keycnt = 0;
    halt_requested = false;
    stop_requested = false;
    bios.latch = 0xE129F000u;
    bios.intr_wait_active = false;
    bios.intr_wait_mask = 0;
}

void Bus::SetKeys(std::uint32_t keys) {
    keyinput = static_cast<std::uint16_t>((~keys) & KeyMask);
    if ((keycnt & (1u << 14)) != 0) {
        const std::uint16_t mask = keycnt & 0x3FFu;
        const std::uint16_t pressed = static_cast<std::uint16_t>(keys);
        const bool cond_and = (keycnt & (1u << 15)) != 0;
        const bool hit =
            cond_and ? ((pressed & mask) == mask && mask != 0) : ((pressed & mask) != 0);
        if (hit) {
            if_flags |= IrqKeypad;
        }
    }
}

bool Bus::Tick(std::uint32_t cycles) {
    const TimerStepResult timer_res = timers.Tick(cycles);
    if_flags |= timer_res.irq_mask;

    if ((timer_res.overflow_mask & 1u) != 0) {
        const auto [d1, d2] = apu.OnTimerOverflow(0);
        if (d1) {
            RunFifoDma(1);
        }
        if (d2) {
            RunFifoDma(2);
        }
    }
    if ((timer_res.overflow_mask & 2u) != 0) {
        const auto [d1, d2] = apu.OnTimerOverflow(1);
        if (d1) {
            RunFifoDma(1);
        }
        if (d2) {
            RunFifoDma(2);
        }
    }

    apu.Tick(cycles);

    const PpuStepEvents ppu_ev = ppu.Step(cycles);
    if_flags |= ppu_ev.irq_mask;
    if (ppu_ev.entered_hblank) {
        RunDma(DmaTrigger::HBlank);
    }
    if (ppu_ev.entered_vblank) {
        RunDma(DmaTrigger::VBlank);
    }
    return ppu_ev.frame_complete;
}

std::uint8_t Bus::ReadU8(std::uint32_t addr) {
    switch (addr >> 24) {
    case 0x00:
        return bios.ReadU8(addr, cpu_pc);
    case 0x02:
        return ewram[static_cast<std::size_t>(addr) & (EwramSize - 1u)];
    case 0x03:
        return iwram[static_cast<std::size_t>(addr) & (IwramSize - 1u)];
    case 0x04: {
        const std::uint16_t half = ReadIoU16(addr & ~1u);
        return ((addr & 1u) == 0) ? static_cast<std::uint8_t>(half & 0xFFu)
                                  : static_cast<std::uint8_t>(half >> 8);
    }
    case 0x05:
        return ppu.palette_ram[static_cast<std::size_t>(addr) & 0x3FFu];
    case 0x06:
        return ppu.vram[VramOffset(addr)];
    case 0x07:
        return ppu.oam[static_cast<std::size_t>(addr) & 0x3FFu];
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0D:
        if (cartridge.IsEepromAccess(addr)) {
            return static_cast<std::uint8_t>(cartridge.ReadEeprom() & 0xFFu);
        }
        return cartridge.ReadRomU8(addr & 0x01FFFFFFu);
    case 0x0E:
    case 0x0F:
        return cartridge.ReadBackupU8(addr);
    default:
        return static_cast<std::uint8_t>((open_bus_latch >> ((addr & 3u) * 8u)) & 0xFFu);
    }
}

std::uint16_t Bus::ReadU16(std::uint32_t addr) {
    const std::uint32_t aligned = addr & ~1u;
    switch (aligned >> 24) {
    case 0x00:
        return bios.ReadU16(aligned, cpu_pc);
    case 0x02:
        return LoadLe16(&ewram[static_cast<std::size_t>(aligned) & (EwramSize - 1u)]);
    case 0x03:
        return LoadLe16(&iwram[static_cast<std::size_t>(aligned) & (IwramSize - 1u)]);
    case 0x04:
        return ReadIoU16(aligned);
    case 0x05:
        return LoadLe16(&ppu.palette_ram[static_cast<std::size_t>(aligned) & 0x3FFu]);
    case 0x06:
        return LoadLe16(&ppu.vram[VramOffset(aligned)]);
    case 0x07:
        return LoadLe16(&ppu.oam[static_cast<std::size_t>(aligned) & 0x3FFu]);
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0D:
        if (cartridge.IsEepromAccess(aligned)) {
            return cartridge.ReadEeprom();
        }
        return cartridge.ReadRomU16(aligned & 0x01FFFFFFu);
    case 0x0E:
    case 0x0F:
        return static_cast<std::uint16_t>(cartridge.ReadBackupU8(aligned) * 0x0101u);
    default:
        return static_cast<std::uint16_t>((open_bus_latch >> ((aligned & 2u) * 8u)) & 0xFFFFu);
    }
}

std::uint32_t Bus::ReadU32(std::uint32_t addr) {
    const std::uint32_t aligned = addr & ~3u;
    switch (aligned >> 24) {
    case 0x00:
        return bios.ReadU32(aligned, cpu_pc);
    case 0x02:
        return LoadLe32(&ewram[static_cast<std::size_t>(aligned) & (EwramSize - 1u)]);
    case 0x03:
        return LoadLe32(&iwram[static_cast<std::size_t>(aligned) & (IwramSize - 1u)]);
    case 0x04: {
        const std::uint32_t lo = ReadIoU16(aligned);
        const std::uint32_t hi = ReadIoU16(aligned + 2u);
        return lo | (hi << 16);
    }
    case 0x05:
        return LoadLe32(&ppu.palette_ram[static_cast<std::size_t>(aligned) & 0x3FFu]);
    case 0x06:
        return LoadLe32(&ppu.vram[VramOffset(aligned)]);
    case 0x07:
        return LoadLe32(&ppu.oam[static_cast<std::size_t>(aligned) & 0x3FFu]);
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0D:
        return cartridge.ReadRomU32(aligned & 0x01FFFFFFu);
    case 0x0E:
    case 0x0F:
        return static_cast<std::uint32_t>(cartridge.ReadBackupU8(aligned)) * 0x01010101u;
    default:
        return open_bus_latch;
    }
}

void Bus::WriteU8(std::uint32_t addr, std::uint8_t val) {
    switch (addr >> 24) {
    case 0x02:
        ewram[static_cast<std::size_t>(addr) & (EwramSize - 1u)] = val;
        break;
    case 0x03:
        iwram[static_cast<std::size_t>(addr) & (IwramSize - 1u)] = val;
        break;
    case 0x04:
        WriteIoU8(addr, val);
        break;
    case 0x05: {
        const std::size_t i = static_cast<std::size_t>(addr) & 0x3FEu;
        ppu.palette_ram[i] = val;
        ppu.palette_ram[i + 1] = val;
        break;
    }
    case 0x06: {
        const std::size_t off = VramOffset(addr & ~1u);
        const std::size_t obj_limit = ((ppu.dispcnt & 0x7u) >= 3u) ? 0x14000u : 0x10000u;
        if (off < obj_limit) {
            ppu.vram[off] = val;
            ppu.vram[off + 1] = val;
        }
        break;
    }
    case 0x07:
        break;
    case 0x0E:
    case 0x0F:
        cartridge.WriteBackupU8(addr, val);
        break;
    default:
        break;
    }
}

void Bus::WriteU16(std::uint32_t addr, std::uint16_t val) {
    const std::uint32_t aligned = addr & ~1u;
    switch (aligned >> 24) {
    case 0x02:
        StoreLe16(&ewram[static_cast<std::size_t>(aligned) & (EwramSize - 1u)], val);
        break;
    case 0x03:
        StoreLe16(&iwram[static_cast<std::size_t>(aligned) & (IwramSize - 1u)], val);
        break;
    case 0x04:
        WriteIoU16(aligned, val);
        break;
    case 0x05:
        StoreLe16(&ppu.palette_ram[static_cast<std::size_t>(aligned) & 0x3FFu], val);
        break;
    case 0x06:
        StoreLe16(&ppu.vram[VramOffset(aligned)], val);
        break;
    case 0x07:
        StoreLe16(&ppu.oam[static_cast<std::size_t>(aligned) & 0x3FFu], val);
        break;
    case 0x08:
    case 0x09:
        cartridge.WriteGpio(aligned & 0x01FFFFFFu, val);
        break;
    case 0x0D:
        if (cartridge.IsEepromAccess(aligned)) {
            cartridge.WriteEeprom(val);
        }
        break;
    case 0x0E:
    case 0x0F:
        cartridge.WriteBackupU8(aligned, static_cast<std::uint8_t>(val));
        break;
    default:
        break;
    }
}

void Bus::WriteU32(std::uint32_t addr, std::uint32_t val) {
    const std::uint32_t aligned = addr & ~3u;
    switch (aligned >> 24) {
    case 0x02:
        StoreLe32(&ewram[static_cast<std::size_t>(aligned) & (EwramSize - 1u)], val);
        break;
    case 0x03:
        StoreLe32(&iwram[static_cast<std::size_t>(aligned) & (IwramSize - 1u)], val);
        break;
    case 0x04:
        WriteIoU32(aligned, val);
        break;
    case 0x05:
        StoreLe32(&ppu.palette_ram[static_cast<std::size_t>(aligned) & 0x3FFu], val);
        break;
    case 0x06:
        StoreLe32(&ppu.vram[VramOffset(aligned)], val);
        break;
    case 0x07:
        StoreLe32(&ppu.oam[static_cast<std::size_t>(aligned) & 0x3FFu], val);
        break;
    case 0x0E:
    case 0x0F:
        cartridge.WriteBackupU8(aligned, static_cast<std::uint8_t>(val));
        break;
    default:
        break;
    }
}

std::uint16_t Bus::ReadIoU16(std::uint32_t addr) const {
    if ((addr & 0x00FFFFFFu) >= 0x400u) {
        return 0;
    }
    const std::uint32_t offset = addr & 0xFFFFu;
    if (offset <= 0x056u) {
        return ppu.ReadIoU16(offset);
    }
    if (offset >= 0x060u && offset <= 0x09Eu) {
        return apu.ReadIoU16(offset);
    }
    if (offset >= 0x0B0u && offset <= 0x0DEu) {
        const std::size_t ch = (offset - 0x0B0u) / 12u;
        const std::uint32_t reg = (offset - 0x0B0u) % 12u;
        return (reg == 10u) ? dma.channels[ch].control : 0u;
    }
    if (offset >= 0x100u && offset <= 0x10Eu) {
        const std::size_t idx = (offset - 0x100u) / 4u;
        const bool is_ctrl = ((offset - 0x100u) & 2u) != 0;
        return timers.ReadU16(idx, is_ctrl);
    }
    switch (offset) {
    case 0x130:
        return keyinput;
    case 0x132:
        return keycnt;
    case 0x200:
        return ie;
    case 0x202:
        return if_flags;
    case 0x204:
        return waitcnt;
    case 0x208:
        return ime;
    case 0x300:
        return postflg;
    default:
        return 0;
    }
}

void Bus::WriteIoU8(std::uint32_t addr, std::uint8_t val) {
    if ((addr & 0x00FFFFFFu) >= 0x400u) {
        return;
    }
    const std::uint32_t offset = addr & 0xFFFFu;
    if (offset == 0x202u) {
        if_flags &= static_cast<std::uint16_t>(~static_cast<std::uint16_t>(val));
        return;
    }
    if (offset == 0x203u) {
        if_flags &= static_cast<std::uint16_t>(~(static_cast<std::uint16_t>(val) << 8));
        return;
    }
    if (offset == 0x300u) {
        postflg = val;
        return;
    }
    if (offset == 0x301u) {
        if ((val & 0x80u) != 0) {
            stop_requested = true;
        } else {
            halt_requested = true;
        }
        return;
    }
    const std::uint32_t aligned = addr & ~1u;
    const std::uint16_t cur = ReadIoU16(aligned);
    const std::uint16_t merged =
        ((addr & 1u) == 0)
            ? static_cast<std::uint16_t>((cur & 0xFF00u) | val)
            : static_cast<std::uint16_t>((cur & 0x00FFu) | (static_cast<std::uint16_t>(val) << 8));
    WriteIoU16(aligned, merged);
}

void Bus::WriteIoU16(std::uint32_t addr, std::uint16_t val) {
    if ((addr & 0x00FFFFFFu) >= 0x400u) {
        return;
    }
    const std::uint32_t offset = addr & 0xFFFFu;
    if (offset <= 0x056u) {
        ppu.WriteIoU16(offset, val);
        return;
    }
    if (offset >= 0x060u && offset <= 0x09Eu) {
        apu.WriteIoU16(offset, val);
        return;
    }
    if (offset >= 0x0B0u && offset <= 0x0DEu) {
        const std::size_t ch = (offset - 0x0B0u) / 12u;
        const std::uint32_t reg = (offset - 0x0B0u) % 12u;
        auto& d = dma.channels[ch];
        switch (reg) {
        case 0:
            d.sad = (d.sad & ~0xFFFFu) | val;
            break;
        case 2:
            d.sad = (d.sad & 0xFFFFu) | (static_cast<std::uint32_t>(val & 0x0FFFu) << 16);
            break;
        case 4:
            d.dad = (d.dad & ~0xFFFFu) | val;
            break;
        case 6:
            d.dad = (d.dad & 0xFFFFu) | (static_cast<std::uint32_t>(val & 0x0FFFu) << 16);
            break;
        case 8:
            d.count = val;
            break;
        case 10:
            if (dma.WriteControl(ch, val)) {
                ExecuteDmaChannel(ch);
            }
            break;
        default:
            break;
        }
        return;
    }
    if (offset >= 0x100u && offset <= 0x10Eu) {
        const std::size_t idx = (offset - 0x100u) / 4u;
        const bool is_ctrl = ((offset - 0x100u) & 2u) != 0;
        if (is_ctrl) {
            timers.WriteControl(idx, val);
        } else {
            timers.WriteReload(idx, val);
        }
        return;
    }
    switch (offset) {
    case 0x132:
        keycnt = val;
        break;
    case 0x200:
        ie = val & 0x3FFFu;
        break;
    case 0x202:
        if_flags &= static_cast<std::uint16_t>(~val);
        break;
    case 0x204:
        waitcnt = val;
        break;
    case 0x208:
        ime = val & 1u;
        break;
    case 0x300:
        postflg = static_cast<std::uint8_t>(val);
        if (((val >> 8) & 0x80u) != 0) {
            stop_requested = true;
        } else {
            halt_requested = true;
        }
        break;
    default:
        break;
    }
}

void Bus::WriteIoU32(std::uint32_t addr, std::uint32_t val) {
    if ((addr & 0x00FFFFFFu) >= 0x400u) {
        return;
    }
    const std::uint32_t offset = addr & 0xFFFFu;
    if (offset == 0x0A0u) {
        apu.fifo_a.PushWord(val);
    } else if (offset == 0x0A4u) {
        apu.fifo_b.PushWord(val);
    } else {
        WriteIoU16(addr, static_cast<std::uint16_t>(val));
        WriteIoU16(addr + 2u, static_cast<std::uint16_t>(val >> 16));
    }
}

void Bus::RunDma(DmaTrigger trigger) {
    for (std::size_t ch = 0; ch < 4; ++ch) {
        if (dma.channels[ch].Enabled() && dma.channels[ch].Timing() == trigger) {
            ExecuteDmaChannel(ch);
        }
    }
}

void Bus::RunFifoDma(std::size_t ch) {
    const DmaChannel c = dma.channels[ch];
    if (c.Enabled() && c.Timing() == DmaTrigger::Special) {
        std::uint32_t src = c.internal_sad & ~3u;
        const std::uint32_t dst = c.internal_dad & ~3u;
        const std::int32_t src_step = c.SrcAdj() * 4;
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t word = ReadU32(src);
            WriteIoU32(dst, word);
            src = static_cast<std::uint32_t>(static_cast<std::int64_t>(src) + src_step);
        }
        dma.channels[ch].internal_sad = src;
        if (c.IrqEnabled()) {
            if_flags |= DmaController::IrqBit(ch);
        }
    }
}

void Bus::ExecuteDmaChannel(std::size_t ch) {
    const DmaChannel c = dma.channels[ch];
    const std::uint32_t count = c.internal_count;
    const bool is_32 = c.Is32Bit();
    const std::int32_t step = is_32 ? 4 : 2;
    const std::int32_t src_step = c.SrcAdj() * step;
    const std::int32_t dst_step = c.DstAdj() * step;

    std::uint32_t src = c.internal_sad & ~static_cast<std::uint32_t>(step - 1);
    std::uint32_t dst = c.internal_dad & ~static_cast<std::uint32_t>(step - 1);

    if (ch == 3 && cartridge.IsEepromAccess(dst)) {
        cartridge.ConfigureEepromDma(count);
    }

    for (std::uint32_t i = 0; i < count; ++i) {
        if (is_32) {
            WriteU32(dst, ReadU32(src));
        } else {
            WriteU16(dst, ReadU16(src));
        }
        src = static_cast<std::uint32_t>(static_cast<std::int64_t>(src) + src_step);
        dst = static_cast<std::uint32_t>(static_cast<std::int64_t>(dst) + dst_step);
    }

    dma.channels[ch].internal_sad = src;
    dma.channels[ch].internal_dad = c.DstReload() ? dma.channels[ch].dad : dst;

    if (c.Repeat() && c.Timing() != DmaTrigger::Immediate) {
        const std::uint32_t raw_cnt = dma.channels[ch].count & (ch == 3 ? 0xFFFFu : 0x3FFFu);
        dma.channels[ch].internal_count =
            (raw_cnt == 0) ? (ch == 3 ? 0x10000u : 0x4000u) : raw_cnt;
    } else {
        dma.channels[ch].control &= static_cast<std::uint16_t>(~(1u << 15));
    }

    if (c.IrqEnabled()) {
        if_flags |= DmaController::IrqBit(ch);
    }
}

} // namespace Emulation::Gba
