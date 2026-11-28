// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "emulation/classic/cartridge.h"
#include "emulation/classic/runtime.h"

namespace {

std::vector<std::uint8_t> GameBoyRom(bool color, std::uint8_t cartridge_type = 0x03) {
    std::vector<std::uint8_t> rom(0x8000);
    constexpr std::array<std::uint8_t, 4> entry{0x00, 0xC3, 0x50, 0x01};
    constexpr std::array<std::uint8_t, 4> signature{0xCE, 0xED, 0x66, 0x66};
    constexpr std::array<std::uint8_t, 12> program{
        0x3E, 0x0A, 0xEA, 0x00, 0x00, 0x3E, 0x5A, 0xEA, 0x00, 0xA0, 0x18, 0xFE,
    };
    std::copy(entry.begin(), entry.end(), rom.begin() + 0x100);
    std::copy(signature.begin(), signature.end(), rom.begin() + 0x104);
    std::copy(program.begin(), program.end(), rom.begin() + 0x150);
    rom[0x143] = color ? 0x80 : 0;
    rom[0x147] = cartridge_type;
    rom[0x149] = 0x02;
    for (std::size_t offset = 0x134; offset < 0x14D; ++offset) {
        rom[0x14D] = static_cast<std::uint8_t>(rom[0x14D] - rom[offset] - 1);
    }
    return rom;
}

std::vector<std::uint8_t> AdvanceRom() {
    std::vector<std::uint8_t> rom(0x8000);
    constexpr std::array<std::uint8_t, 8> entry{0x2E, 0x00, 0x00, 0xEA, 0x24, 0xFF, 0xAE, 0x51};
    constexpr std::array<std::uint8_t, 4> loop{0xFE, 0xFF, 0xFF, 0xEA};
    std::copy(entry.begin(), entry.end(), rom.begin());
    std::copy(loop.begin(), loop.end(), rom.begin() + 0xC0);
    rom[0xB2] = 0x96;
    rom[0xBD] = 0x51;
    return rom;
}

} // namespace

TEST_CASE("Classic runtime produces packed video and stereo audio for each handheld",
          "[classic-runtime]") {
    std::string error;
    auto gb = Emulation::Classic::Runtime::Open(GameBoyRom(false), {}, error);
    INFO(error);
    REQUIRE(gb);
    auto gbc = Emulation::Classic::Runtime::Open(GameBoyRom(true), {}, error);
    INFO(error);
    REQUIRE(gbc);
    auto gba = Emulation::Classic::Runtime::Open(AdvanceRom(), {}, error);
    INFO(error);
    REQUIRE(gba);

    REQUIRE(gb->Width() == 160);
    REQUIRE(gb->Height() == 144);
    REQUIRE(gbc->Width() == 160);
    REQUIRE(gbc->Height() == 144);
    REQUIRE(gba->Width() == 240);
    REQUIRE(gba->Height() == 160);
    for (auto* runtime : {gb.get(), gbc.get(), gba.get()}) {
        REQUIRE(runtime->FrameRate() > 59.0);
        REQUIRE(runtime->FrameRate() < 61.0);
        for (int frame = 0; frame < 4; ++frame) {
            runtime->RunFrame(0);
        }
        REQUIRE(runtime->Video().size() == runtime->Width() * runtime->Height());
        REQUIRE(!runtime->Audio().empty());
        REQUIRE(runtime->Audio().size() % 2 == 0);
    }
}

TEST_CASE("Classic runtime restores and flushes raw battery saves", "[classic-runtime]") {
    std::string error;
    auto runtime = Emulation::Classic::Runtime::Open(GameBoyRom(false), {}, error);
    INFO(error);
    REQUIRE(runtime);
    for (int frame = 0; frame < 4; ++frame) {
        runtime->RunFrame(0);
    }
    const auto saved = runtime->Save();
    REQUIRE(saved.size() == 0x2000);
    REQUIRE(saved.front() == 0x5A);
    REQUIRE(runtime->Finish() == saved);

    auto restored = Emulation::Classic::Runtime::Open(GameBoyRom(false), saved, error);
    INFO(error);
    REQUIRE(restored);
    REQUIRE(restored->Save() == saved);
}

TEST_CASE("Classic runtime snapshots the cartridge clock before stopping", "[classic-runtime]") {
    std::string error;
    auto runtime = Emulation::Classic::Runtime::Open(GameBoyRom(false, 0x10), {}, error);
    INFO(error);
    REQUIRE(runtime);
    runtime->RunFrame(0);
    const auto saved = runtime->Save();
    REQUIRE(saved.size() == 0x2000 + 48);
    REQUIRE(saved.front() == 0x5A);
    REQUIRE(runtime->Finish().size() == saved.size());
}

TEST_CASE("Classic runtime rejects invalid cartridges and oversized saves", "[classic-runtime]") {
    std::string error;
    REQUIRE_FALSE(Emulation::Classic::Runtime::Open({0, 1, 2}, {}, error));
    REQUIRE_FALSE(error.empty());
    std::vector<std::uint8_t> oversized_save(512 * 1024);
    REQUIRE_FALSE(Emulation::Classic::Runtime::Open(GameBoyRom(false), oversized_save, error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("C++ GBA runtime supports save states, rewind, cheats, video options, and SRAM",
          "[classic-runtime]") {
    auto rom = AdvanceRom();
    constexpr std::array<std::uint8_t, 6> sram_tag{'S', 'R', 'A', 'M', '_', 'V'};
    std::copy(sram_tag.begin(), sram_tag.end(), rom.begin() + 0x200);
    // Program at 0xC0:
    // mov r0, #0x04000000; mov r1, #0x403; strh r1, [r0];
    // mov r2, #0x06000000; mov r3, #0x001F; strh r3, [r2];
    // mov r4, #0x0E000000; mov r5, #0x5A; strb r5, [r4]; b .
    constexpr std::array<std::uint32_t, 11> prog{
        0xE3A00404u, 0xE3A01003u, 0xE3811B01u, 0xE1C010B0u,
        0xE3A02406u, 0xE3A0301Fu, 0xE1C230B0u,
        0xE3A0440Eu, 0xE3A0505Au, 0xE5C45000u, 0xEAFFFFFEu,
    };
    for (std::size_t i = 0; i < prog.size(); ++i) {
        const auto off = 0xC0 + i * 4;
        rom[off] = static_cast<std::uint8_t>(prog[i] & 0xFF);
        rom[off + 1] = static_cast<std::uint8_t>((prog[i] >> 8) & 0xFF);
        rom[off + 2] = static_cast<std::uint8_t>((prog[i] >> 16) & 0xFF);
        rom[off + 3] = static_cast<std::uint8_t>((prog[i] >> 24) & 0xFF);
    }

    std::string error;
    auto runtime = Emulation::Classic::Runtime::Open(rom, {}, error);
    INFO(error);
    REQUIRE(runtime);
    REQUIRE(runtime->SupportsGbaFeatures());

    runtime->RunFrame(0);
    REQUIRE(runtime->Video().front() == 0xFF0000FFu);
    const auto saved = runtime->Save();
    REQUIRE(saved.size() == 0x8000);
    REQUIRE(saved.front() == 0x5A);

    runtime->SetVideoOptions(true, false);
    REQUIRE(runtime->Video().front() != 0xFF0000FFu);
    runtime->SetVideoOptions(false, false);
    REQUIRE(runtime->Video().front() == 0xFF0000FFu);

    std::vector<std::uint8_t> state;
    REQUIRE(runtime->SaveState(state, error));
    REQUIRE_FALSE(state.empty());

    REQUIRE(runtime->AddCheat("06000000:03E0", error));
    runtime->RunFrame(0);
    REQUIRE(runtime->Video().front() == 0xFF00FF00u);

    REQUIRE(runtime->RewindFrame(error));
    REQUIRE(runtime->Video().front() == 0xFF0000FFu);

    runtime->ClearCheats();
    runtime->Reset();
    REQUIRE(runtime->LoadState(state, error));
    REQUIRE(runtime->Video().front() == 0xFF0000FFu);
}

TEST_CASE("Classic cartridge metadata extracts title, publisher, region, version, and hardware",
          "[classic-runtime]") {
    std::vector<std::uint8_t> gb(0x8000);
    constexpr std::array<std::uint8_t, 4> gb_sig{0xCE, 0xED, 0x66, 0x66};
    std::copy(gb_sig.begin(), gb_sig.end(), gb.begin() + 0x104);
    constexpr std::string_view gb_title = "POCKET DEMO";
    std::copy(gb_title.begin(), gb_title.end(), gb.begin() + 0x134);
    gb[0x143] = 0x00;
    gb[0x147] = 0x03; // MBC1 + RAM + Battery
    gb[0x14A] = 0x01; // Overseas
    gb[0x14B] = 0x01; // Nintendo (old licensee)
    gb[0x14C] = 0x02; // 1.2
    for (std::size_t offset = 0x134; offset < 0x14D; ++offset) {
        gb[0x14D] = static_cast<std::uint8_t>(gb[0x14D] - gb[offset] - 1);
    }

    const auto gb_meta = Emulation::Classic::ReadClassicCartridgeMetadata(
        "gb", gb.size(), [&gb](std::span<std::uint8_t> out) {
            std::copy_n(gb.begin(), out.size(), out.begin());
            return out.size();
        });
    REQUIRE(gb_meta);
    REQUIRE(gb_meta->system == Emulation::Classic::ClassicCartridge::GameBoy);
    REQUIRE(gb_meta->internal_title == "POCKET DEMO");
    REQUIRE(gb_meta->publisher == "Nintendo");
    REQUIRE(gb_meta->region == "Overseas");
    REQUIRE(gb_meta->version == "1.2");
    REQUIRE(gb_meta->hardware == "MBC1 + RAM + Battery");
    REQUIRE(gb_meta->file_size == gb.size());

    std::vector<std::uint8_t> gbc(0x8000);
    std::copy(gb_sig.begin(), gb_sig.end(), gbc.begin() + 0x104);
    constexpr std::string_view gbc_title = "COLOR TEST";
    std::copy(gbc_title.begin(), gbc_title.end(), gbc.begin() + 0x134);
    constexpr std::string_view gbc_code = "BCDE";
    std::copy(gbc_code.begin(), gbc_code.end(), gbc.begin() + 0x13F);
    gbc[0x143] = 0xC0;
    gbc[0x144] = '0';
    gbc[0x145] = '8'; // Capcom
    gbc[0x147] = 0x1B; // MBC5 + RAM + Battery
    gbc[0x14A] = 0x00; // Japan
    gbc[0x14B] = 0x33; // Use new licensee
    gbc[0x14C] = 0x00;
    for (std::size_t offset = 0x134; offset < 0x14D; ++offset) {
        gbc[0x14D] = static_cast<std::uint8_t>(gbc[0x14D] - gbc[offset] - 1);
    }

    const auto gbc_meta = Emulation::Classic::ReadClassicCartridgeMetadata(
        "gbc", gbc.size(), [&gbc](std::span<std::uint8_t> out) {
            std::copy_n(gbc.begin(), out.size(), out.begin());
            return out.size();
        });
    REQUIRE(gbc_meta);
    REQUIRE(gbc_meta->system == Emulation::Classic::ClassicCartridge::GameBoyColor);
    REQUIRE(gbc_meta->internal_title == "COLOR TEST");
    REQUIRE(gbc_meta->game_code == "BCDE");
    REQUIRE(gbc_meta->publisher == "Capcom");
    REQUIRE(gbc_meta->region == "Japan");
    REQUIRE(gbc_meta->hardware == "MBC5 + RAM + Battery");

    auto gba = AdvanceRom();
    constexpr std::string_view gba_title = "ADVANCE DEMO";
    std::copy(gba_title.begin(), gba_title.end(), gba.begin() + 0xA0);
    constexpr std::string_view gba_code = "AGBE";
    std::copy(gba_code.begin(), gba_code.end(), gba.begin() + 0xAC);
    gba[0xB0] = '0';
    gba[0xB1] = '1'; // Nintendo
    gba[0xBC] = 0x01;
    std::uint8_t gba_sum = 0;
    for (std::size_t offset = 0xA0; offset < 0xBD; ++offset) {
        gba_sum = static_cast<std::uint8_t>(gba_sum - gba[offset]);
    }
    gba[0xBD] = static_cast<std::uint8_t>(gba_sum - 0x19u);

    const auto gba_meta = Emulation::Classic::ReadClassicCartridgeMetadata(
        "gba", gba.size(), [&gba](std::span<std::uint8_t> out) {
            std::copy_n(gba.begin(), out.size(), out.begin());
            return out.size();
        });
    REQUIRE(gba_meta);
    REQUIRE(gba_meta->system == Emulation::Classic::ClassicCartridge::GameBoyAdvance);
    REQUIRE(gba_meta->internal_title == "ADVANCE DEMO");
    REQUIRE(gba_meta->game_code == "AGBE");
    REQUIRE(gba_meta->region == "USA");
    REQUIRE(gba_meta->publisher == "Nintendo");
    REQUIRE(gba_meta->version == "1.1");
    REQUIRE(gba_meta->file_size == gba.size());
}
