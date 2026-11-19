// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

using Shader::Ampere::Instruction;

TEST_CASE("TXD register pressure covers the layer and a single destination",
          "[video_core][ampere][logged_numeric]") {
    // dimension at bit 61, second destination at bit 64, channel mask at bit 72.
    const auto txd = [](u32 dest, u32 coordinates, u32 dimension, u32 mask) {
        return Instruction{{0xb6c | (dest << 16) | (coordinates << 24), (dimension << 29), 255U | (mask << 8), 0}};
    };
    // ARRAY_2D reads three coordinates: the layer follows the pair in the same group.
    auto pressure = Shader::Ampere::PrepareRegisterPressure(txd(0, 216, 5, 0x3), 216);
    REQUIRE(pressure.valid);
    CHECK(pressure.instruction.Bits(24, 8) < 216);
    // Straddling a group boundary borrows two contiguous low groups so coordinates stay consecutive.
    pressure = Shader::Ampere::PrepareRegisterPressure(txd(0, 218, 5, 0x3), 216);
    REQUIRE(pressure.valid);
    CHECK(pressure.prefix.size() == 16);
    CHECK(pressure.suffix.size() == 16);
    CHECK(pressure.instruction.Bits(24, 8) + 2 < 216);
    CHECK(Shader::Ampere::PrepareRegisterPressure(txd(0, 218, 1, 0x3), 216).valid);
    // Single and paired destinations near the end of a group also map contiguously below reserved.
    pressure = Shader::Ampere::PrepareRegisterPressure(txd(247, 0, 1, 0x1), 216);
    REQUIRE(pressure.valid);
    CHECK(pressure.instruction.Bits(16, 8) < 216);
    pressure = Shader::Ampere::PrepareRegisterPressure(txd(247, 0, 1, 0x3), 216);
    REQUIRE(pressure.valid);
    CHECK(pressure.prefix.size() == 16);
    CHECK(pressure.instruction.Bits(16, 8) + 1 < 216);
    // Register spans that would reach RZ (255) are still rejected.
    CHECK_FALSE(Shader::Ampere::PrepareRegisterPressure(txd(254, 0, 1, 0x3), 216).valid);
}

TEST_CASE("PrepareRegisterPressure reserves contiguous blocks for consecutive paired groups",
          "[video_core][ampere][logged_numeric]") {
    const auto txd_full = [](u32 dest, u32 coordinates, u32 gradients, u32 dimension, u32 mask) {
        return Instruction{
            {0xb6c | (dest << 16) | (coordinates << 24), gradients | (dimension << 29), 255U | (mask << 8), 0}};
    };

    // Rd=R215..R216 (groups 53..54) and Ra=R219..R221 (groups 54..55) form a 3-quartet paired run,
    // while Rb=R208..R211 occupies the quartet immediately below group 53.
    const auto pressure = Shader::Ampere::PrepareRegisterPressure(txd_full(215, 219, 208, 5, 0x3), 216);
    REQUIRE(pressure.valid);
    REQUIRE(pressure.prefix.size() == 24);
    REQUIRE(pressure.suffix.size() == 24);

    std::array<bool, 256> borrowed{};
    for (size_t i = 0; i < pressure.prefix.size(); i += 2) {
        const u32 low = static_cast<u32>(pressure.prefix[i] & 0xffU);
        REQUIRE(low < 216);
        CHECK_FALSE(borrowed[low]);
        borrowed[low] = true;
        // Must not overlap Rb (R208..R211) or the original high groups (R212..R223).
        CHECK((low < 208 || low >= 212));
        CHECK((low < 212 || low >= 224));
    }
    const u32 mapped_dest = pressure.instruction.Bits(16, 8);
    const u32 mapped_coords = pressure.instruction.Bits(24, 8);
    CHECK(mapped_dest + 1 < 216);
    CHECK(mapped_coords + 2 < 216);
    CHECK(mapped_coords == mapped_dest + 4);
    CHECK(pressure.instruction.Bits(32, 8) == 208);

    // An 8-register operand (opcode 0x83b, count=8) spanning 3 consecutive groups also reserves 12 regs.
    const Instruction wide_op{{0x83bU | (215U << 16) | (208U << 24), 0, 3U << 8, 0}};
    const auto wide_pressure = Shader::Ampere::PrepareRegisterPressure(wide_op, 216);
    REQUIRE(wide_pressure.valid);
    REQUIRE(wide_pressure.prefix.size() == 24);
    CHECK(wide_pressure.instruction.Bits(16, 8) + 7 < 216);
    for (size_t i = 0; i < wide_pressure.prefix.size(); i += 2) {
        const u32 low = static_cast<u32>(wide_pressure.prefix[i] & 0xffU);
        CHECK(low < 216);
        CHECK(low != 208);
    }

    // Reject when only 8 contiguous low registers fit (would alias Rb=R8..R11 or exceed reserved=16).
    CHECK_FALSE(Shader::Ampere::PrepareRegisterPressure(txd_full(15, 19, 8, 5, 0x3), 16).valid);
    CHECK_FALSE(Shader::Ampere::PrepareRegisterPressure(txd_full(15, 19, 4, 5, 0x3), 16).valid);
}

TEST_CASE("Uniform-base STG Ra=254 is allowed with 32-bit offset and rejected with 64-bit offset",
          "[video_core][ampere][logged_numeric]") {
    constexpr Instruction EXIT{{0x0000794d, 0, 0x03800000, 0}};
    // 0x986 STG.E [UR0 + R254], R0 (32-bit offset when bit 90 is 0; size=4 at bits 73..75, bit 72=1).
    const Instruction stg_32bit_ra{{0x986U | (0x7U << 12) | (254U << 24), 0, 0x0810a900U, 0}};
    const auto pressure_32 = Shader::Ampere::PrepareRegisterPressure(stg_32bit_ra, 216);
    REQUIRE(pressure_32.valid);
    CHECK(pressure_32.instruction.Bits(24, 8) < 216);
    const auto lowered = Shader::Ampere::LowerToMaxwell(std::array<Instruction, 2>{{stg_32bit_ra, EXIT}});
    CHECK(static_cast<bool>(lowered));

    // With bit 90 set (64-bit offset), Ra=254 would reach RZ (255) and must be rejected.
    const Instruction stg_64bit_ra{{0x986U | (0x7U << 12) | (254U << 24), 0, 0x0c10a900U, 0}};
    CHECK_FALSE(Shader::Ampere::PrepareRegisterPressure(stg_64bit_ra, 216).valid);
}
