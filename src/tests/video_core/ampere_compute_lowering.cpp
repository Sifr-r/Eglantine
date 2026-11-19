// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

constexpr Instruction UniformLea(u32 guard, u32 carry) {
    // ULEA UR12, UP(carry), UR11, UR19, 0x3. Predicate 7 discards the carry.
    return {{0x291 | (guard << 12) | (12U << 16) | (11U << 24), 19,
             63 | (3U << 11) | (carry << 17) | (1U << 27), 0}};
}

TEST_CASE("Ampere guarded ULEA restores the guest predicate", "[video_core][ampere]") {
    constexpr u32 NegatedUP2 = 8 | 2;
    const auto lowered = LowerToMaxwell(std::array{UniformLea(NegatedUP2, 7), Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    const auto& words = lowered.instructions;
    const auto lea = std::ranges::find_if(words, [](u64 word) { return (word >> 48) == 0x5c18; });
    REQUIRE(lea != words.end());
    CHECK((*lea & 255) == 228); // UR12
    CHECK(((*lea >> 8) & 255) == 227); // UR11
    CHECK(((*lea >> 20) & 255) == 235); // UR19
    CHECK(((*lea >> 16) & 15) == (8 | 4)); // UP2 is temporarily loaded into P4
    CHECK(((*lea >> 39) & 31) == 3);

    const auto loads_predicates_from = [](u64 word, u32 reg) {
        return (word >> 48) == 0x38f0 && ((word >> 8) & 255) == reg &&
               ((word >> 20) & 255) == (1 << 4);
    };
    CHECK(std::any_of(words.begin(), lea,
                      [&](u64 word) { return loads_predicates_from(word, 239); }));
    CHECK(std::any_of(lea + 1, words.end(),
                      [&](u64 word) { return loads_predicates_from(word, 253); }));
}

TEST_CASE("Ampere ULEA writes its carry to the selected uniform predicate",
          "[video_core][ampere]") {
    const auto lowered = LowerToMaxwell(std::array{UniformLea(7, 1), Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    const auto& words = lowered.instructions;
    const auto compare = std::ranges::find_if(words, [](u64 word) { return (word >> 52) == 0x5b6; });
    REQUIRE(compare != words.end());
    CHECK(((*compare >> 3) & 7) == 5); // P5 is the temporary for UP1
    CHECK(std::any_of(compare + 1, words.end(), [](u64 word) {
        return (word >> 48) == 0x38e8 && (word & 255) == 239 &&
               ((word >> 20) & 255) == (1 << 5);
    }));
}

TEST_CASE("Ampere surface reductions preserve every U32 atomic operation",
          "[video_core][ampere]") {
    for (u32 op = 0; op < 8; ++op) {
        if (op == 3 || op == 4) continue;
        for (const u32 dimension : {1U, 3U}) {
            for (const u32 guard : {1U, 7U, 9U}) {
                const Instruction instruction{{0x0400059fU | (guard << 12),
                    0x00008400U | (dimension << 29), 0x0010e000U | (op << 23), 0}};
                const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
                INFO(lowered.error);
                REQUIRE(lowered);
                REQUIRE(lowered.required_texture_buffer == 0);
                CHECK(lowered.instructions[0] == (0xea000000000004ffULL | (u64{guard} << 16) |
                    (u64{op} << 29) | (u64{dimension} << 33) | (0x84ULL << 36)));
                auto invalid = instruction;
                invalid.words[2] |= 2U << 9;
                CHECK_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
            }
        }
    }
}

TEST_CASE("Half per-tap gathers prove offsets across an unrelated LEA.HI",
          "[video_core][ampere][gather]") {
    const Instruction lea{{0x08087211, 0x00000007, 0x078ff8ff, 0x000fc800}};
    const Instruction gather{{0x0a0c7b63, 0x30003000, 0x001eaf0d, 0x000fc200}};
    auto code = std::array{
        Instruction{{0x00007802, 0xff00ff00, 0xf00, 0}},
        Instruction{{0x00017802, 0x01000001, 0xf00, 0}},
        lea, gather, Exit,
    };
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(lowered);
    code[2].words[0] = 0x08017211;
    CHECK(LowerToMaxwell(code).error == "native per-tap gather nonconstant offsets");
}
} // namespace
