// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <optional>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/program_override.h"

namespace {

using Shader::Ampere::AppendNativeAddressMap;
using Shader::Ampere::DecodeProgramOverride;
using Shader::Ampere::EncodeProgramOverride;
using Shader::Ampere::HasNativeAddressMap;
using Shader::Ampere::Instruction;
using Shader::Ampere::LoweredProgram;
using Shader::Ampere::LoweringOptions;
using Shader::Ampere::LowerToMaxwell;
using Shader::Ampere::NativeProgramAddress;
using Shader::Ampere::NativeProgramKey;
using Shader::Ampere::ProgramCaptureState;
using Shader::Ampere::ProgramOverrideSourceSize;

constexpr Instruction kExit{{0x0000794dU, 0U, 0x03800000U, 0U}};
constexpr Instruction kNop{{0x00007918U, 0U, 0U, 0U}};

void CheckLowering(std::span<const Instruction> code, LoweringOptions options = {}) {
    const auto lowered = LowerToMaxwell(code, options);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    CHECK(lowered.source_size == code.size_bytes());
    CHECK(lowered.source_instruction_offsets.size() == code.size());
}

void CheckLoweringError(std::span<const Instruction> code, u32 error_offset = 0) {
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE_FALSE(static_cast<bool>(lowered));
    CHECK_FALSE(lowered.error.empty());
    CHECK(lowered.error_offset == error_offset);
    CHECK(lowered.instructions.empty());
    CHECK(lowered.source_instruction_offsets.empty());
}

} // namespace

TEST_CASE("Ampere program lowering reports valid and invalid input", "[video_core][ampere]") {
    SECTION("Valid programs across lowering options and edge cases") {
        // Minimal EXIT
        CheckLowering(std::array{kExit});

        // CALL + main EXIT + RET relocation
        CheckLowering(std::array<Instruction, 4>{{
            {{0x00097802U, 0x20U, 0x00000f00U, 0U}},
            {{0x00007944U, 0x10U, 0x03c00000U, 0U}},
            kExit,
            {{0x09007950U, 0xffffffc0U, 0x03c3ffffU, 0U}},
        }});

        // Register pressure shadow copies (R240..R244)
        CheckLowering(std::array<Instruction, 2>{{
            {{0x221U | (7U << 12) | (240U << 16) | (242U << 24), 244U, 0U, 0U}},
            kExit,
        }});

        // UR28+ retry loop (high_uniform_registers)
        CheckLowering(std::array<Instruction, 2>{{
            {{0x291U | (7U << 12) | (30U << 16) | (11U << 24), 19U,
              63U | (3U << 11) | (7U << 17) | (1U << 27), 0U}},
            kExit,
        }});

        // Geometry + native_code_address
        CheckLowering(
            std::array<Instruction, 5>{{
                {{0x0004734eU, 0U, 0U, 0U}},
                {{0x00017923U, 0U, 0U, 0U}},
                {{0x00027324U, 0xffU, 0x4000U, 0U}},
                {{0x02037924U, 0U, 0x8000U, 0U}},
                kExit,
            }},
            {.geometry = true, .native_code_address = 0x00000012fffffff0ULL});

        // HMMA inline and compact tensor modes
        const std::array<Instruction, 2> hmma{{
            {{0x0004723cU, 0x00000008U, 0U, 0U}},
            kExit,
        }};
        CheckLowering(hmma, {.tensor = true, .compact_tensor = false});
        CheckLowering(hmma, {.tensor = true, .compact_tensor = true});

        // ULDC, I2F, LEA + STG
        CheckLowering(std::array<Instruction, 3>{{
            {{0x00047ab9U, 0x01800800U, 0x00000400U, 0x000fe200U}},
            {{0x00017306U, 0x00000001U, 0x00101400U, 0x020ea200U}},
            kExit,
        }});
        CheckLowering(std::array<Instruction, 3>{{
            {{0x08087211U, 0x00000007U, 0x078ff8ffU, 0x000fc800U}},
            {{0x986U | (7U << 12) | (254U << 24), 0U, 0x0810a900U, 0U}},
            kExit,
        }});

        // F2I, I2F variants, VOTE, SURED
        CheckLowering(std::array<Instruction, 3>{{
            {{0x00067305U, 0x00000009U, 0x0021b100U, 0U}},
            {{0x00037306U, 0x00000003U, 0x00101400U, 0x000e2400U}},
            kExit,
        }});
        CheckLowering(std::array<Instruction, 3>{{
            {{0x00007886U, 0U, 0x038e0100U, 0U}},
            {{0x0000759fU, 0xa0002203U, 0x0010e000U, 0x0011e600U}},
            kExit,
        }});
    }

    SECTION("Rejected programs report an error and offset") {
        CheckLoweringError({});
        CheckLoweringError(std::array{kNop}, sizeof(Instruction));
        // Out-of-range forward branch
        CheckLoweringError(std::array<Instruction, 2>{{
            {{0x00007947U, 0x40U, 0x00000000U, 0U}},
            kExit,
        }}, 2 * sizeof(Instruction));
        // Callee overwrites link register
        CheckLoweringError(std::array<Instruction, 4>{{
            {{0x00097802U, 0x20U, 0x00000f00U, 0U}},
            {{0x00007944U, 0x10U, 0x03c00000U, 0U}},
            kExit,
            {{0x00097802U, 0x01U, 0x00000f00U, 0U}},
        }}, 3 * sizeof(Instruction));
        // Unsupported F2I, LDC, SURED overflow, VOTE, and unknown opcode
        CheckLoweringError(std::array<Instruction, 2>{{
            {{0x00067305U, 0x00000009U, 0x0020b001U, 0U}},
            kExit,
        }});
        CheckLoweringError(std::array<Instruction, 2>{{
            {{0x02157b82U, 0U, 5U << 9, 0U}},
            kExit,
        }});
        CheckLoweringError(std::array<Instruction, 2>{{
            {{0x02147b82U, 0U, 6U << 9, 0U}},
            kExit,
        }});
        CheckLoweringError(std::array<Instruction, 2>{{
            {{0xfd00759fU, 0xa0002203U, 0x0010e000U, 0x0011e600U}},
            kExit,
        }});
        CheckLoweringError(std::array<Instruction, 2>{{
            {{0x001c7886U, 0U, 0x038e0100U, 0U}},
            kExit,
        }});
        CheckLoweringError(std::array<Instruction, 2>{{
            {{0x00007fffU, 0U, 0U, 0U}},
            kExit,
        }});
    }
}

TEST_CASE("Ampere program capture tracks terminal instructions", "[video_core][ampere]") {
    const std::vector<std::vector<Instruction>> sequences{
        {kExit},
        {kNop, kExit},
        // Forward branch over first EXIT
        {Instruction{{0x00007947U, 16U, 0U, 0U}}, kExit, kExit},
        // CALL + main EXIT + RET
        {Instruction{{0x00007944U, 16U, 0U, 0U}}, kExit,
         Instruction{{0x00007950U, 0xffffffc0U, 0x03c3ffffU, 0U}}},
        // Computed branch (BRX) + self-branch BRA
        {Instruction{{0x00007949U, 0U, 0U, 0U}}, kExit,
         Instruction{{0x00007947U, static_cast<u32>(-16), 0x0003ffffU, 0U}}},
        // Invalid negative branch target
        {Instruction{{0x00007947U, static_cast<u32>(-32), 0x0003ffffU, 0U}}},
        // Invalid unaligned JMP target
        {Instruction{{0x00007945U, 4U, 0U, 0U}}},
        // Static IADD3.X geometry exit-envelope program
        {
            Instruction{{0x0000734eU, 0U, 0U, 0x000fe400U}},
            Instruction{{0x00037802U, 0x70U, 0x00000f00U, 0x000fc800U}},
            Instruction{{0x00007210U, 3U, 0x07f1e0ffU, 0x000fc800U}},
            Instruction{{0x01017210U, 0xffU, 0x007fe4ffU, 0x000fc800U}},
            Instruction{{0x00007357U, 0U, 0U, 0x0001e400U}},
            Instruction{{0x0000894dU, 0U, 0x03800000U, 0x021fea00U}},
            Instruction{{0x0000794dU, 0U, 0x03800000U, 0x003fea00U}},
            Instruction{{0x00ff7324U, 0xffU, 0U, 0x0001e200U}},
            Instruction{{0x00007957U, 0U, 0U, 0x000fe200U}},
            Instruction{{0x0000794dU, 0U, 0x03800000U, 0x001fea00U}},
        },
        // IMAD geometry exit-envelope program
        {
            Instruction{{0x0000734eU, 0U, 0U, 0x000fe400U}},
            Instruction{{0x00037802U, 0x70U, 0x00000f00U, 0x000fc800U}},
            Instruction{{0x00007210U, 3U, 0x07f1e0ffU, 0x000fca00U}},
            Instruction{{0xff017224U, 0xffU, 0x000e0401U, 0x000fc800U}},
            Instruction{{0x00007357U, 0U, 0U, 0x0001e400U}},
            kExit,
            kNop,
            Instruction{{0x00ff7324U, 0xffU, 0U, 0x0001e200U}},
            Instruction{{0x00007957U, 0U, 0U, 0x000fe200U}},
            kExit,
        },
        // Geometry exit-envelope with body CALL + BRX + forward target past handler
        {
            Instruction{{0x0000734eU, 0U, 0U, 0x000fe400U}},
            Instruction{{0x00037802U, 0x90U, 0x00000f00U, 0x000fc800U}},
            Instruction{{0x00007210U, 3U, 0x07f1e0ffU, 0x000fc800U}},
            Instruction{{0x01017210U, 0xffU, 0x007fe4ffU, 0x000fc800U}},
            Instruction{{0x00007357U, 0U, 0U, 0x0001e400U}},
            Instruction{{0x00007944U, 0x10U, 0x03c00000U, 0x000fea00U}},
            Instruction{{0x00007949U, 0U, 0U, 0U}},
            Instruction{{0x08007950U, 0xffffff80U, 0x03c3ffffU, 0x000fea00U}},
            Instruction{{0x00007947U, 0x90U, 0x03800000U, 0U}},
            Instruction{{0x00ff7324U, 0xffU, 0U, 0x0001e200U}},
            Instruction{{0x00007957U, 0U, 0U, 0x000fe200U}},
            kExit,
        },
        // Invalid geometry exit handler destination
        {
            Instruction{{0x0000734eU, 0U, 0U, 0x000fe400U}},
            Instruction{{0x00037802U, 0x40U, 0x00000f00U, 0x000fc800U}},
            Instruction{{0x00007210U, 3U, 0x07f1e0ffU, 0x000fc800U}},
            Instruction{{0x01017210U, 0xffU, 0x007fe4ffU, 0x000fc800U}},
            Instruction{{0x00007357U, 0U, 0U, 0x0001e400U}},
        },
        // Mismatched geometry exit setup form stops at first main EXIT
        {
            Instruction{{0x0000634eU, 0U, 0U, 0x000fe400U}},
            Instruction{{0x00037802U, 0x70U, 0x00000f00U, 0x000fc800U}},
            Instruction{{0x00007210U, 3U, 0x07f1e0ffU, 0x000fc800U}},
            Instruction{{0x01017210U, 0xffU, 0x007fe4ffU, 0x000fc800U}},
            Instruction{{0x00007357U, 0U, 0U, 0x0001e400U}},
            kExit,
        },
    };

    using Result = ProgramCaptureState::Result;
    const std::array expected{
        Result::Complete, Result::Complete, Result::Complete, Result::Complete, Result::Complete,
        Result::InvalidTarget, Result::InvalidTarget, Result::Complete, Result::Complete,
        Result::Complete, Result::InvalidTarget, Result::Complete,
    };
    REQUIRE(sequences.size() == expected.size());
    for (size_t i = 0; i < sequences.size(); ++i) {
        CAPTURE(i);
        const auto& sequence = sequences[i];
        ProgramCaptureState capture;
        const auto last = static_cast<u32>(sequence.size() - 1);
        for (u32 index = 0; index < last; ++index) {
            CAPTURE(index);
            CHECK(capture.Observe(sequence[index], index) == Result::Continue);
        }
        CHECK(capture.Observe(sequence[last], last) == expected[i]);
    }
}

TEST_CASE("Ampere native address map resolves aligned addresses and rejects corrupt entries",
          "[video_core][ampere]") {
    std::vector<u64> mapped(16, 0);
    CHECK_FALSE(HasNativeAddressMap(mapped));
    CHECK(NativeProgramAddress(mapped, 0) == std::nullopt);

    AppendNativeAddressMap(mapped, std::array<u32, 3>{0, 2, 4});
    REQUIRE(HasNativeAddressMap(mapped));
    CHECK(NativeProgramAddress(mapped, 0) == 0x50U);
    CHECK(NativeProgramAddress(mapped, 16) == 0x68U);
    CHECK(NativeProgramAddress(mapped, 32) == 0x78U);
    CHECK(NativeProgramAddress(mapped, 4) == std::nullopt);
    CHECK(NativeProgramAddress(mapped, 48) == std::nullopt);

    auto corrupt_target = mapped;
    corrupt_target[corrupt_target.size() - 3] = 0x60;
    CHECK(NativeProgramAddress(corrupt_target, 32) == std::nullopt);

    auto corrupt_count = mapped;
    corrupt_count[corrupt_count.size() - 2] = corrupt_count.size();
    CHECK(NativeProgramAddress(corrupt_count, 0) == std::nullopt);
}

TEST_CASE("Ampere native override round trip and malformed input", "[video_core][ampere]") {
    const std::array<Instruction, 3> source{{
        {{0x00017306U, 0x00000001U, 0x00101400U, 0x020ea200U}},
        {{0x00047ab9U, 0x01800800U, 0x00000400U, 0x000fe200U}},
        kExit,
    }};

    CHECK(NativeProgramKey({}) == 14695981039346656037ULL);
    CHECK(NativeProgramKey(source) == 0x1bcfc3cdef7f2776ULL);

    const LoweredProgram lowered = LowerToMaxwell(source);
    REQUIRE(static_cast<bool>(lowered));

    const auto encoded = EncodeProgramOverride(source, lowered);
    REQUIRE(ProgramOverrideSourceSize(encoded) == sizeof(source));

    LoweredProgram invalid;
    invalid.error = "failed";
    CHECK(EncodeProgramOverride(source, invalid).empty());

    const LoweredProgram decoded = DecodeProgramOverride(encoded, source);
    REQUIRE(static_cast<bool>(decoded));
    CHECK(decoded.instructions == lowered.instructions);
    CHECK(decoded.source_instruction_offsets == lowered.source_instruction_offsets);
    CHECK(decoded.required_texture_buffer == lowered.required_texture_buffer);
    CHECK(decoded.source_size == lowered.source_size);

    auto truncated = encoded;
    truncated.pop_back();
    auto bad_magic = encoded;
    bad_magic[0] ^= 1U;
    for (const auto& malformed : {std::vector<u8>{}, std::vector<u8>{1, 2, 3, 4},
                                  truncated, bad_magic}) {
        CHECK(ProgramOverrideSourceSize(malformed) == 0);
        const auto rejected = DecodeProgramOverride(malformed, source);
        CHECK(rejected.error == "invalid native override header or length");
    }

    auto bad_source = source;
    bad_source[0].words[0] ^= 1U;
    CHECK(DecodeProgramOverride(encoded, bad_source).error == "native override source mismatch");
}
