// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "emulation/gba/cartridge.h"
#include "emulation/gba/emulator.h"
#include "emulation/gba/types.h"

namespace {

std::vector<std::uint8_t> BuildGbaRom(std::span<const std::uint32_t> instructions) {
    std::vector<std::uint8_t> rom(0x8000, 0);
    // Branch from 0x0800_0000 -> 0x0800_00C0: offset = (0xC0 - 8)/4 = 0x2E
    constexpr std::array<std::uint8_t, 4> entry{0x2E, 0x00, 0x00, 0xEA};
    constexpr std::array<std::uint8_t, 4> logo{0x24, 0xFF, 0xAE, 0x51};
    std::copy(entry.begin(), entry.end(), rom.begin());
    std::copy(logo.begin(), logo.end(), rom.begin() + 0x04);
    rom[0xB2] = 0x96;
    std::uint8_t sum = 0x19;
    for (std::size_t i = 0xA0; i < 0xBD; ++i) {
        sum = static_cast<std::uint8_t>(sum + rom[i]);
    }
    rom[0xBD] = static_cast<std::uint8_t>(0u - sum);

    for (std::size_t i = 0; i < instructions.size(); ++i) {
        const std::size_t off = 0xC0 + i * 4;
        const std::uint32_t insn = instructions[i];
        rom[off] = static_cast<std::uint8_t>(insn & 0xFFu);
        rom[off + 1] = static_cast<std::uint8_t>((insn >> 8) & 0xFFu);
        rom[off + 2] = static_cast<std::uint8_t>((insn >> 16) & 0xFFu);
        rom[off + 3] = static_cast<std::uint8_t>((insn >> 24) & 0xFFu);
    }
    return rom;
}

} // namespace

TEST_CASE("GBA Mode 3 renders pixels and supports color correction and blending", "[gba-core]") {
    constexpr std::array<std::uint32_t, 8> insns{
        0xE3A00301u, // mov r0, #0x04000000
        0xE3A01003u, // mov r1, #0x0003
        0xE3811B01u, // orr r1, r1, #0x0400
        0xE1C010B0u, // strh r1, [r0]
        0xE3A02406u, // mov r2, #0x06000000
        0xE3A0301Fu, // mov r3, #0x001F (red)
        0xE1C230B0u, // strh r3, [r2]
        0xEAFFFFFEu, // b .
    };
    auto rom = BuildGbaRom(insns);
    std::string error;
    auto emu = Emulation::Gba::GbaEmulator::Create(std::move(rom), {}, {}, error);
    INFO(error);
    REQUIRE(emu);
    emu->RunFrame(0);

    // Raw red RGB555 (0x1F) -> 0xFF0000FF (0xFFBBGGRR)
    REQUIRE(emu->Video().front() == 0xFF0000FFu);

    // Enable color correction -> red is attenuated/corrected from 255
    emu->SetVideoOptions({true, false});
    REQUIRE(emu->Video().front() != 0xFF0000FFu);
    REQUIRE((emu->Video().front() & 0xFFu) > 180u);

    // Enable interframe blending after frame 2
    emu->SetVideoOptions({false, true});
    emu->RunFrame(0);
    REQUIRE(emu->Video().front() == 0xFF0000FFu);
}

TEST_CASE("GBA Thumb ALU and SRAM battery save work", "[gba-core]") {
    constexpr std::array<std::uint32_t, 6> insns{
        0xE28F0004u, // add r0, pc, #4 (0xC0+8+4 = 0xCC)
        0xE2800001u, // add r0, r0, #1
        0xE12FFF10u, // bx r0
        0x215A4801u, // ldr r0, [pc, #4] | movs r1, #0x5A
        0xE7FE7001u, // strb r1, [r0, #0] | b .
        0x0E000000u, // literal 0x0E000000
    };
    auto rom = BuildGbaRom(insns);
    constexpr std::array<std::uint8_t, 6> sram_tag{'S', 'R', 'A', 'M', '_', 'V'};
    std::copy(sram_tag.begin(), sram_tag.end(), rom.begin() + 0x200);

    std::string error;
    auto emu = Emulation::Gba::GbaEmulator::Create(rom, {}, {}, error);
    INFO(error);
    REQUIRE(emu);
    emu->RunFrame(0);
    const auto save = emu->SaveData();
    REQUIRE(save.size() == 32 * 1024);
    REQUIRE(save[0] == 0x5Au);

    auto restored = Emulation::Gba::GbaEmulator::Create(std::move(rom), save, {}, error);
    INFO(error);
    REQUIRE(restored);
    REQUIRE(restored->SaveData()[0] == 0x5Au);
}

TEST_CASE("GBA keypad, audio, savestate, rewind, and cheats work", "[gba-core]") {
    constexpr std::array<std::uint32_t, 15> insns{
        0xE3A00402u, // mov r0, #0x02000000
        0xE3A01404u, // mov r1, #0x04000000
        0xE2814E13u, // add r4, r1, #0x130
        0xE3A02080u, // mov r2, #0x80
        0xE1C128B4u, // strh r2, [r1, #0x84] (SOUNDCNT_X)
        0xE3A02C77u, // mov r2, #0x7700
        0xE3822011u, // orr r2, r2, #0x11
        0xE1C128B0u, // strh r2, [r1, #0x80] (SOUNDCNT_L)
        0xE3A02CF0u, // mov r2, #0xF000
        0xE1C126B2u, // strh r2, [r1, #0x62]
        0xE3A02C84u, // mov r2, #0x8400
        0xE1C126B4u, // strh r2, [r1, #0x64]
        0xE1D430B0u, // ldrh r3, [r4]
        0xE1C030B0u, // strh r3, [r0]
        0xEAFFFFFCu, // b -4 insns
    };
    auto rom = BuildGbaRom(insns);
    std::string error;
    auto emu = Emulation::Gba::GbaEmulator::Create(std::move(rom), {}, {}, error);
    INFO(error);
    REQUIRE(emu);

    emu->RunFrame(Emulation::Gba::KeyA);
    REQUIRE(emu->GetBus().ReadU16(0x02000000u) == 0x3FEu);
    REQUIRE(!emu->Audio().empty());
    REQUIRE(emu->Audio().size() % 2 == 0);
    REQUIRE(std::any_of(emu->Audio().begin(), emu->Audio().end(),
                        [](std::int16_t s) { return s != 0; }));

    std::vector<std::uint8_t> state;
    REQUIRE(emu->SaveState(state, error));

    for (int i = 0; i < 3; ++i) {
        emu->RunFrame(0);
    }
    REQUIRE(emu->GetBus().ReadU16(0x02000000u) == 0x3FFu);

    for (int i = 0; i < 3; ++i) {
        REQUIRE(emu->RewindFrame(error));
    }
    REQUIRE(emu->GetBus().ReadU16(0x02000000u) == 0x3FEu);

    auto bad_state = state;
    bad_state[25] ^= 0xFFu;
    REQUIRE_FALSE(emu->LoadState(bad_state, error));

    REQUIRE(emu->LoadState(state, error));
    REQUIRE(emu->GetBus().ReadU16(0x02000000u) == 0x3FEu);

    REQUIRE(emu->AddCheat("02000010:BEEF", error));
    REQUIRE(emu->AddCheat("02000020 CAFEBABE", error));
    emu->RunFrame(0);
    REQUIRE(emu->GetBus().ReadU16(0x02000010u) == 0xBEEFu);
    REQUIRE(emu->GetBus().ReadU32(0x02000020u) == 0xCAFEBABEu);
}

TEST_CASE("GBA PPU Mode 4, DMA, Flash 128K, and HLE SWI work", "[gba-core]") {
    constexpr std::array<std::uint32_t, 17> insns{
        0xE3A00064u, // mov r0, #100
        0xE3A01007u, // mov r1, #7
        0xEF060000u, // swi 0x06 (Div)
        0xE3A02402u, // mov r2, #0x02000000
        0xE5820000u, // str r0, [r2]
        0xE5821004u, // str r1, [r2, #4]
        0xE3A03404u, // mov r3, #0x04000000
        0xE3A04004u, // mov r4, #4
        0xE3844B01u, // orr r4, r4, #0x400
        0xE1C340B0u, // strh r4, [r3] (DISPCNT = 0x0404)
        0xE3A03405u, // mov r3, #0x05000000
        0xE3A04FF8u, // mov r4, #0x3E0 (green)
        0xE1C340B2u, // strh r4, [r3, #2] (Palette[1])
        0xE3A03406u, // mov r3, #0x06000000
        0xE3A04001u, // mov r4, #1
        0xE5C34000u, // strb r4, [r3] (VRAM[0] = 1)
        0xEAFFFFFEu, // b .
    };
    auto rom = BuildGbaRom(insns);
    constexpr std::array<std::uint8_t, 9> flash_tag{'F', 'L', 'A', 'S', 'H', '1', 'M', '_', 'V'};
    std::copy(flash_tag.begin(), flash_tag.end(), rom.begin() + 0x200);

    std::string error;
    auto emu = Emulation::Gba::GbaEmulator::Create(std::move(rom), {}, {}, error);
    INFO(error);
    REQUIRE(emu);
    emu->RunFrame(0);

    REQUIRE(emu->GetBus().ReadU32(0x02000000u) == 14u);
    REQUIRE(emu->GetBus().ReadU32(0x02000004u) == 2u);
    REQUIRE(emu->Video().front() == 0xFF00FF00u);

    // Flash 128K bank switch & byte write
    emu->GetBus().WriteU8(0x0E005555u, 0xAAu);
    emu->GetBus().WriteU8(0x0E002AAAu, 0x55u);
    emu->GetBus().WriteU8(0x0E005555u, 0xB0u);
    emu->GetBus().WriteU8(0x0E000000u, 0x01u);
    emu->GetBus().WriteU8(0x0E005555u, 0xAAu);
    emu->GetBus().WriteU8(0x0E002AAAu, 0x55u);
    emu->GetBus().WriteU8(0x0E005555u, 0xA0u);
    emu->GetBus().WriteU8(0x0E000000u, 0x3Cu);
    const auto flash = emu->SaveData();
    REQUIRE(flash.size() == 128 * 1024);
    REQUIRE(flash[0x10000] == 0x3Cu);

    // Immediate DMA transfer from EWRAM[0] -> EWRAM[0x100]
    emu->GetBus().WriteU32(0x040000D4u, 0x02000000u);
    emu->GetBus().WriteU32(0x040000D8u, 0x02000100u);
    emu->GetBus().WriteU16(0x040000DCu, 2u);
    emu->GetBus().WriteU16(0x040000DEu, 0x8400u);
    REQUIRE(emu->GetBus().ReadU32(0x02000100u) == 14u);
    REQUIRE(emu->GetBus().ReadU32(0x02000104u) == 2u);
}

TEST_CASE("GBA audit regressions: VBlankIntrWait, LDM^, IF byte write, EEPROM, cheats, savestate",
          "[gba-core]") {
    SECTION("SWI 0x05 VBlankIntrWait wakes on VBlank IRQ and advances") {
        // Handler at 0x0800_0200 (in ROM): acknowledges VBlank in IF and 0x03007FF8, then returns.
        // Main at 0x0800_00C0:
        //   0x00C0: set UserIrqPtr (0x03007FFC) = 0x08000200
        //   0x00D0: enable VBlank IRQ in DISPSTAT (0x04000004 |= 8)
        //   0x00E0: swi 0x05 (VBlankIntrWait)
        //   0x00E4: write 0xCAFEBABE to 0x02000040
        //   0x00F4: b .
        auto rom = BuildGbaRom({});
        auto put_u32 = [&rom](std::size_t off, std::uint32_t v) {
            rom[off] = static_cast<std::uint8_t>(v & 0xFFu);
            rom[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
            rom[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
            rom[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
        };
        // Main code at 0xC0:
        // ldr r0, [pc, #0x34] (0x080000FC -> 0x03007FFC)
        // ldr r1, [pc, #0x34] (0x08000100 -> 0x08000200)
        // str r1, [r0]
        // mov r2, #0x04000000
        // mov r3, #0x08
        // strh r3, [r2, #4]
        // swi 0x05
        // ldr r0, [pc, #0x24] (0x08000104 -> 0x02000040)
        // ldr r1, [pc, #0x24] (0x08000108 -> 0xCAFEBABE)
        // str r1, [r0]
        // b .
        const std::array<std::uint32_t, 15> main_insns{
            0xE59F0034u, // 0x00C0: ldr r0, [pc, #0x34] -> 0x00FC
            0xE59F1034u, // 0x00C4: ldr r1, [pc, #0x34] -> 0x0100
            0xE5801000u, // 0x00C8: str r1, [r0]
            0xE3A02404u, // 0x00CC: mov r2, #0x04000000
            0xE3A03008u, // 0x00D0: mov r3, #0x08
            0xE1C230B4u, // 0x00D4: strh r3, [r2, #4]
            0xEF050000u, // 0x00D8: swi 0x05
            0xE59F0020u, // 0x00DC: ldr r0, [pc, #0x20] -> 0x0104
            0xE59F1020u, // 0x00E0: ldr r1, [pc, #0x20] -> 0x0108
            0xE5801000u, // 0x00E4: str r1, [r0]
            0xEAFFFFFEu, // 0x00E8: b .
            0x00000000u, // 0x00EC: nop
            0x00000000u, // 0x00F0: nop
            0x00000000u, // 0x00F4: nop
            0x00000000u, // 0x00F8: nop
        };
        for (std::size_t i = 0; i < main_insns.size(); ++i) {
            put_u32(0xC0 + i * 4, main_insns[i]);
        }
        put_u32(0xFC, 0x03007FFCu);
        put_u32(0x100, 0x08000200u);
        put_u32(0x104, 0x02000040u);
        put_u32(0x108, 0xCAFEBABEu);

        // IRQ handler at 0x200:
        //   mov r0, #0x04000000
        //   orr r0, r0, #0x200
        //   mov r1, #1
        //   strh r1, [r0, #2]      (ack IF VBlank at 0x04000202)
        //   ldr r0, =0x03007FF8
        //   ldrh r2, [r0]
        //   orr r2, r2, #1
        //   strh r2, [r0]
        //   bx lr
        const std::array<std::uint32_t, 9> irq_insns{
            0xE3A00404u, // 0x200: mov r0, #0x04000000
            0xE3800C02u, // 0x204: orr r0, r0, #0x200
            0xE3A01001u, // 0x208: mov r1, #1
            0xE1C010B2u, // 0x20C: strh r1, [r0, #2]
            0xE59F0010u, // 0x210: ldr r0, [pc, #0x10] -> 0x228
            0xE1D020B0u, // 0x214: ldrh r2, [r0]
            0xE3822001u, // 0x218: orr r2, r2, #1
            0xE1C020B0u, // 0x21C: strh r2, [r0]
            0xE12FFF1Eu, // 0x220: bx lr
        };
        for (std::size_t i = 0; i < irq_insns.size(); ++i) {
            put_u32(0x200 + i * 4, irq_insns[i]);
        }
        put_u32(0x228, 0x03007FF8u);

        std::string error;
        auto emu = Emulation::Gba::GbaEmulator::Create(std::move(rom), {}, {}, error);
        REQUIRE(emu);
        emu->RunFrame(0);
        emu->RunFrame(0);
        REQUIRE(emu->GetBus().ReadU32(0x02000040u) == 0xCAFEBABEu);
    }

    SECTION("ARM LDMFD SP!, {R0, PC}^ preserves User SP and updates IRQ SP") {
        auto rom = BuildGbaRom({});
        std::string error;
        auto emu = Emulation::Gba::GbaEmulator::Create(std::move(rom), {}, {}, error);
        REQUIRE(emu);
        auto& bus = emu->GetBus();
        Emulation::Gba::CpuRegs regs(false);
        regs.r[13] = 0x03007F00u;
        regs.sp_usr = 0x03007F00u;
        regs.SwitchMode(Emulation::Gba::CpuMode::Irq);
        regs.spsr_irq = 0x1Fu; // System mode
        regs.r[13] = 0x03007F98u;
        bus.WriteU32(0x03007F98u, 0x11223344u);
        bus.WriteU32(0x03007F9Cu, 0x08000100u);
        // LDMFD SP!, {R0, PC}^ -> 0xE8FD8001
        (void)Emulation::Gba::ExecArm(0xE8FD8001u, regs, bus);
        REQUIRE(regs.Mode() == Emulation::Gba::CpuMode::System);
        REQUIRE(regs.r[0] == 0x11223344u);
        REQUIRE(regs.r[13] == 0x03007F00u);
        REQUIRE(regs.sp_irq == 0x03007FA0u);
    }

    SECTION("8-bit write to IF clears only the addressed byte") {
        auto rom = BuildGbaRom({});
        std::string error;
        auto emu = Emulation::Gba::GbaEmulator::Create(std::move(rom), {}, {}, error);
        REQUIRE(emu);
        auto& bus = emu->GetBus();
        bus.if_flags = Emulation::Gba::IrqVBlank | Emulation::Gba::IrqDma0;
        bus.WriteU8(0x04000202u, 0x01u);
        REQUIRE(bus.if_flags == Emulation::Gba::IrqDma0);
    }

    SECTION("Corrupted EEPROM savestate snapshot is rejected without OOB read") {
        auto rom = BuildGbaRom({});
        constexpr std::array<std::uint8_t, 8> eeprom_tag{'E', 'E', 'P', 'R', 'O', 'M', '_', 'V'};
        std::copy(eeprom_tag.begin(), eeprom_tag.end(), rom.begin() + 0x200);
        std::string error;
        Emulation::Gba::Cartridge cart;
        REQUIRE(Emulation::Gba::Cartridge::Create(std::move(rom), {}, cart, error));
        auto snap = cart.StateSnapshot();
        REQUIRE(snap.size() > 13);
        snap[5] = static_cast<std::uint8_t>(Emulation::Gba::EepromPhase::ReadBits);
        snap[12] = 255u; // out-of-bounds bit index
        REQUIRE_FALSE(cart.RestoreSnapshot(snap, error));
    }

    SECTION("CheatEngine::AddCode is transactional on multi-line failure") {
        Emulation::Gba::CheatEngine cheats;
        std::string error;
        REQUIRE(cheats.AddCode("02000010:BEEF", error));
        REQUIRE(cheats.Size() == 1);
        REQUIRE_FALSE(cheats.AddCode("02000014:1234\nNOT_A_CHEAT", error));
        REQUIRE(cheats.Size() == 1);
    }

    SECTION("Savestate preserves Ppu::in_hblank and APU PSG state") {
        auto rom = BuildGbaRom({});
        std::string error;
        auto emu = Emulation::Gba::GbaEmulator::Create(std::move(rom), {}, {}, error);
        REQUIRE(emu);
        auto& bus = emu->GetBus();
        bus.WriteU16(0x04000084u, 0x80u);
        bus.WriteU16(0x04000080u, 0x7711u);
        bus.WriteU16(0x04000062u, 0xF000u);
        bus.WriteU16(0x04000064u, 0x8400u);
        (void)bus.Tick(1000u); // enter HBlank
        REQUIRE(bus.ppu.in_hblank);

        std::vector<std::uint8_t> state;
        REQUIRE(emu->SaveState(state, error));

        emu->Reset();
        REQUIRE_FALSE(emu->GetBus().ppu.in_hblank);
        REQUIRE(emu->LoadState(state, error));
        REQUIRE(emu->GetBus().ppu.in_hblank);
        REQUIRE((emu->GetBus().apu.ReadIoU16(0x84u) & 1u) == 1u);
    }
}
