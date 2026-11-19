// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

TEST_CASE("Ampere F2I round up preserves conversion mode", "[video_core][ampere]") {
    using Shader::Ampere::Instruction;
    using Shader::Ampere::LowerToMaxwell;
    const Instruction exit{{0x0000794d, 0, 0x03800000, 0}};

    for (const bool is_signed : {false, true}) {
        const Instruction convert{{0x00067305, 0x00000009,
                                   is_signed ? 0x0021b100U : 0x0020b000U, 0}};
        const auto lowered = LowerToMaxwell(std::array{convert, exit});
        REQUIRE(static_cast<bool>(lowered));
        const auto instruction = std::ranges::find_if(lowered.instructions, [](u64 word) {
            return (word & 0xfff0000000000000ULL) == 0x5cb0000000000000ULL;
        });
        REQUIRE(instruction != lowered.instructions.end());
        REQUIRE((*instruction & 0xff) == 6);
        REQUIRE(((*instruction >> 8) & 3) == 2);
        REQUIRE(((*instruction >> 10) & 3) == 2);
        REQUIRE(((*instruction >> 12) & 1) == static_cast<u64>(is_signed));
        REQUIRE(((*instruction >> 39) & 3) == 2);
        REQUIRE(((*instruction >> 44) & 1) == static_cast<u64>(is_signed));
    }

    const Instruction unsupported{{0x00067305, 0x00000009, 0x0020b001, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unsupported, exit})));
}

TEST_CASE("Ampere F2I signed byte clamps after conversion", "[video_core][ampere]") {
    using Shader::Ampere::Instruction;
    using Shader::Ampere::LowerToMaxwell;
    const Instruction convert{{0x00097305, 0x00000005, 0x0021e100, 0}};
    const Instruction exit{{0x0000794d, 0, 0x03800000, 0}};
    const auto lowered = LowerToMaxwell(std::array{convert, exit});
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.size() == 3);

    const u64 f2i = lowered.instructions[0];
    REQUIRE((f2i & 0xff) == 9);
    REQUIRE(((f2i >> 8) & 3) == 1);
    REQUIRE(((f2i >> 10) & 3) == 2);
    REQUIRE(((f2i >> 12) & 1) == 1);
    REQUIRE(((f2i >> 39) & 3) == 3);
    REQUIRE(((f2i >> 44) & 1) == 1);

    const u64 i2i = lowered.instructions[1];
    REQUIRE((i2i & 0xff) == 9);
    REQUIRE(((i2i >> 8) & 3) == 0);
    REQUIRE(((i2i >> 10) & 3) == 1);
    REQUIRE(((i2i >> 12) & 3) == 3);
    REQUIRE(((i2i >> 20) & 0xff) == 9);
    REQUIRE(((i2i >> 50) & 1) == 1);
}

TEST_CASE("Ampere F2I applies abs and neg to constant-buffer sources", "[video_core][ampere]") {
    using Shader::Ampere::Instruction;
    using Shader::Ampere::LowerToMaxwell;
    const Instruction exit{{0x0000794d, 0, 0x03800000, 0}};
    // nvdisasm prints bit 62 as |c[..]| and bit 63 as -c[..] (U16 and truncating word forms).
    for (const u32 mode : {0x00212800U, 0x0021f100U}) {
        for (const u32 modifiers : {0U, 1U, 2U, 3U}) {
            const Instruction f2i{{0x00157b05, 0x00400400 | (modifiers << 30), mode, 0x0001e400}};
            const auto lowered = LowerToMaxwell(std::array{f2i, exit});
            INFO(lowered.error);
            REQUIRE(static_cast<bool>(lowered));
            const auto has = [&](u64 instruction) {
                return std::ranges::find(lowered.instructions, instruction) !=
                       lowered.instructions.end();
            };
            CHECK(has(0x0400000000070000ULL | 252 | (252ULL << 8) | (0x7fffffffULL << 20)) ==
                  ((modifiers & 1) != 0));
            CHECK(has(0x0400000000070000ULL | 252 | (252ULL << 8) | (0x80000000ULL << 20) |
                      (2ULL << 53)) == ((modifiers & 2) != 0));
        }
    }
}

TEST_CASE("Ampere guarded absolute F2I keeps nearest rounding to U16", "[video_core][ampere]") {
    using Shader::Ampere::Instruction;
    using Shader::Ampere::LowerToMaxwell;
    constexpr u32 NegatedP4 = 8 | 4;
    constexpr u32 Destination = 17;
    constexpr u32 Bank = 3;
    constexpr u32 ByteOffset = 0x44;
    const Instruction convert{{0xb05 | (NegatedP4 << 12) | (Destination << 16),
                               (ByteOffset << 6) | (Bank << 22) | (1U << 30), 0x00212800, 0}};
    const Instruction exit{{0x0000794d, 0, 0x03800000, 0}};
    const auto lowered = LowerToMaxwell(std::array{convert, exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    REQUIRE(lowered.source_instruction_offsets.size() == 2);
    const auto conversion_end = lowered.instructions.begin() + lowered.source_instruction_offsets[1];
    const auto conversion = std::find_if(lowered.instructions.begin(), conversion_end, [](u64 word) {
        return (word >> 52) == 0x5cb;
    });
    REQUIRE(conversion != conversion_end);
    CHECK((*conversion & 255) == Destination);
    CHECK(((*conversion >> 8) & 3) == 1); // U16 destination width
    CHECK(((*conversion >> 12) & 1) == 0); // Unsigned destination
    CHECK(((*conversion >> 39) & 3) == 0); // Round to nearest
    CHECK(std::all_of(lowered.instructions.begin(), conversion_end, [](u64 word) {
        return ((word >> 16) & 15) == NegatedP4;
    }));
}
