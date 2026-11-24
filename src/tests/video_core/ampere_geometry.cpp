// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
using Shader::Ampere::LoweringOptions;
using Shader::Ampere::ProgramCaptureState;
constexpr Instruction exit_inst{{0x0000794d, 0, 0x03800000, 0}};

bool Contains(const Shader::Ampere::LoweredProgram& lowered, u64 instruction) {
    return std::find(lowered.instructions.begin(), lowered.instructions.end(), instruction) !=
           lowered.instructions.end();
}

constexpr std::array<Instruction, 5> geometry_program{{
    {{0x0004734e, 0, 0, 0}},          // LEPC R4
    {{0x00017923, 0, 0, 0}},          // ISBERD R1, [R0]
    {{0x00027324, 0xff, 0x4000, 0}},  // OUT.EMIT R2, R0, RZ
    {{0x02037924, 0, 0x8000, 0}},     // OUT.CUT R3, R2, 0
    exit_inst,
}};
} // namespace

TEST_CASE("Ampere geometry program counter uses the native address", "[video_core][ampere]") {
    const Instruction nop{{0x00007918, 0, 0, 0}};
    const Instruction lepc{{0x0014734e, 0, 0, 0}};
    const std::array code{nop, lepc, exit_inst};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    const auto result = LowerToMaxwell(code, LoweringOptions{
        .geometry = true, .native_code_address = 0x00000012fffffff0ULL});
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.instructions.size() == 4);
    REQUIRE((result.instructions[1] & 0xff) == 20);
    REQUIRE(((result.instructions[1] >> 20) & 0xffffffffULL) == 0);
    REQUIRE((result.instructions[2] & 0xff) == 21);
    REQUIRE(((result.instructions[2] >> 20) & 0xffffffffULL) == 0x13);

    Instruction unsupported = lepc;
    unsupported.words[1] = 1;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unsupported, exit_inst},
                                                   {.geometry = true})));
}

TEST_CASE("Ampere geometry stage map and exit setup are stage specific", "[video_core][ampere]") {
    const Instruction map_read{{0x05127923, 0, 0, 0}};
    const Instruction exit_setup{{0x03007357, 0, 0, 0}};
    const std::array code{map_read, exit_setup, exit_inst};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    const auto result = LowerToMaxwell(code, {.geometry = true});
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.instructions.size() == 2);
    REQUIRE((result.instructions[0] & 0xff) == 18);
    REQUIRE(((result.instructions[0] >> 20) & 0xff) == 5);

    Instruction unsupported = exit_setup;
    unsupported.words[1] = 1;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unsupported, exit_inst},
                                                   {.geometry = true})));
}

TEST_CASE("Ampere F2I supports 16-bit truncation", "[video_core][ampere]") {
    for (const bool is_signed : {false, true}) {
        const Instruction convert{{0x00067305, 9, 0x0021e800U | (is_signed ? 0x100U : 0), 0}};
        const auto result = LowerToMaxwell(std::array{convert, exit_inst});
        REQUIRE(static_cast<bool>(result));
        const auto word = std::ranges::find_if(result.instructions, [](u64 value) {
            return (value & 0xfff0000000000000ULL) == 0x5cb0000000000000ULL;
        });
        REQUIRE(word != result.instructions.end());
        REQUIRE(((*word >> 8) & 3) == 1);
        REQUIRE(((*word >> 12) & 1) == static_cast<u64>(is_signed));
        REQUIRE(((*word >> 39) & 3) == 3);
        REQUIRE(((*word >> 44) & 1) == 1);
    }
    const Instruction unsupported{{0x00067305, 9, 0x0021e801, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unsupported, exit_inst})));
}

TEST_CASE("Ampere geometry programs lower their output instructions", "[video_core][ampere]") {
    const auto lowered = LowerToMaxwell(geometry_program, {.geometry = true});
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(Contains(lowered, 0x010000000007f004ULL)); // MOV32I R4, 0 (its own address)
    REQUIRE(Contains(lowered, 0x010000000007f005ULL)); // MOV32I R5, 0
    REQUIRE(Contains(lowered, 0x5c98078000070001ULL)); // MOV R1, R0
    REQUIRE(Contains(lowered, 0xfbe000800ff70002ULL)); // OUT.EMIT R2, R0, RZ
    REQUIRE(Contains(lowered, 0xf6e0010000070203ULL)); // OUT.CUT R3, R2, 0
}

TEST_CASE("Ampere output instructions stay rejected outside geometry programs",
          "[video_core][ampere]") {
    const auto lowered = LowerToMaxwell(geometry_program);
    REQUIRE(!static_cast<bool>(lowered));
    REQUIRE(lowered.error == "unsupported native opcode");
}

namespace {
constexpr std::array<Instruction, 10> ExitHandlerProgram{{
    {{0x0000734e, 0, 0, 0x000fe400}},
    {{0x00037802, 0x70, 0x00000f00, 0x000fc800}},
    {{0x00007210, 3, 0x07f1e0ff, 0x000fc800}},
    {{0x01017210, 0xff, 0x007fe4ff, 0x000fc800}},
    {{0x00007357, 0, 0, 0x0001e400}},
    {{0x0000894d, 0, 0x03800000, 0x021fea00}},
    {{0x0000794d, 0, 0x03800000, 0x003fea00}},
    {{0x00ff7324, 0xff, 0, 0x0001e200}},
    {{0x00007957, 0, 0, 0x000fe200}},
    {{0x0000794d, 0, 0x03800000, 0x001fea00}},
}};

constexpr auto BananzaExitHandlerProgram = [] {
    std::array<Instruction, 0x740 / 16> code{};
    code.fill(Instruction{{0x00007918, 0, 0, 0x000fc000}});
    std::copy_n(ExitHandlerProgram.begin(), 5, code.begin());
    code[1].words[1] = 0x710;
    code[2].words[3] = 0x000fca00;
    code[3] = Instruction{{0xff017224, 0xff, 0x000e0401, 0x000fc800}};
    code[0x6e0 / 16] = ExitHandlerProgram[6];
    code[0x6f0 / 16] = Instruction{{0xff007322, 0x0003a0ff, 0, 0x0001e400}};
    code[0x700 / 16] = ExitHandlerProgram[6];
    std::copy_n(ExitHandlerProgram.begin() + 7, 3, code.begin() + 0x710 / 16);
    return code;
}();
} // namespace

TEST_CASE("Ampere IMAD exit capture includes the complete Bananza handler",
          "[video_core][ampere]") {
    ProgramCaptureState capture;
    for (u32 index = 0; index < BananzaExitHandlerProgram.size(); ++index) {
        const auto expected = index + 1 == BananzaExitHandlerProgram.size()
                                  ? ProgramCaptureState::Result::Complete
                                  : ProgramCaptureState::Result::Continue;
        REQUIRE(capture.Observe(BananzaExitHandlerProgram[index], index) == expected);
    }
}

TEST_CASE("Ampere IMAD exit capture requires exact carry fields", "[video_core][ampere]") {
    for (u32 word = 0; word < 3; ++word) {
        for (u32 bit = 0; bit < 32; ++bit) {
            auto code = BananzaExitHandlerProgram;
            code[3].words[word] ^= 1U << bit;
            ProgramCaptureState capture;
            for (u32 index = 0; index <= 0x6e0 / 16; ++index) {
                const auto expected = index == 0x6e0 / 16
                                          ? ProgramCaptureState::Result::Complete
                                          : ProgramCaptureState::Result::Continue;
                REQUIRE(capture.Observe(code[index], index) == expected);
            }
        }
    }
}

TEST_CASE("Ampere IMAD exit capture bounds the handler destination", "[video_core][ampere]") {
    for (const u32 target : {0U, 0x40U, 0x711U, ProgramCaptureState::MaximumInstructions * 16}) {
        auto code = BananzaExitHandlerProgram;
        code[1].words[1] = target;
        ProgramCaptureState capture;
        for (u32 index = 0; index < 4; ++index)
            REQUIRE(capture.Observe(code[index], index) == ProgramCaptureState::Result::Continue);
        REQUIRE(capture.Observe(code[4], 4) == ProgramCaptureState::Result::InvalidTarget);
    }
}

TEST_CASE("Ampere IMAD exit capture validates extended instruction modes",
          "[video_core][ampere]") {
    for (u32 setup = 0; setup < 5; ++setup) {
        for (u32 bit : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 30U, 31U}) {
            auto code = BananzaExitHandlerProgram;
            code[setup].words[3] ^= 1U << bit;
            ProgramCaptureState capture;
            for (u32 index = 0; index <= 0x6e0 / 16; ++index) {
                const auto expected = index == 0x6e0 / 16
                                          ? ProgramCaptureState::Result::Complete
                                          : ProgramCaptureState::Result::Continue;
                REQUIRE(capture.Observe(code[index], index) == expected);
            }
        }
    }
}

TEST_CASE("Ampere IMAD exit capture ignores scheduling fields", "[video_core][ampere]") {
    for (u32 setup = 0; setup < 5; ++setup) {
        for (u32 bit = 9; bit < 30; ++bit) {
            auto code = BananzaExitHandlerProgram;
            code[setup].words[3] ^= 1U << bit;
            ProgramCaptureState capture;
            for (u32 index = 0; index < code.size(); ++index) {
                const auto expected = index + 1 == code.size()
                                          ? ProgramCaptureState::Result::Complete
                                          : ProgramCaptureState::Result::Continue;
                REQUIRE(capture.Observe(code[index], index) == expected);
            }
        }
    }
}

TEST_CASE("Ampere IMAD exit capture does not bypass unsupported lowering",
          "[video_core][ampere]") {
    const auto lowered = Shader::Ampere::LowerToMaxwell(BananzaExitHandlerProgram);
    REQUIRE_FALSE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.empty());
    REQUIRE(lowered.error_offset == 0);
}

TEST_CASE("Ampere capture includes the static geometry exit handler", "[video_core][ampere]") {
    ProgramCaptureState capture;
    for (u32 index = 0; index < ExitHandlerProgram.size(); ++index) {
        const auto expected = index + 1 == ExitHandlerProgram.size()
                                  ? ProgramCaptureState::Result::Complete
                                  : ProgramCaptureState::Result::Continue;
        REQUIRE(capture.Observe(ExitHandlerProgram[index], index) == expected);
    }
}

TEST_CASE("Ampere capture bounds the static exit destination", "[video_core][ampere]") {
    for (const u32 target : {0U, 0x40U, 0x71U, ProgramCaptureState::MaximumInstructions * 16}) {
        auto code = ExitHandlerProgram;
        code[1].words[1] = target;
        ProgramCaptureState capture;
        for (u32 index = 0; index < 4; ++index)
            REQUIRE(capture.Observe(code[index], index) == ProgramCaptureState::Result::Continue);
        REQUIRE(capture.Observe(code[4], 4) == ProgramCaptureState::Result::InvalidTarget);
    }
}

TEST_CASE("Ampere capture does not guess other exit setup forms", "[video_core][ampere]") {
    for (u32 setup = 0; setup < 5; ++setup) {
        auto code = ExitHandlerProgram;
        code[setup].words[0] ^= 1U << 12;
        ProgramCaptureState capture;
        for (u32 index = 0; index < 6; ++index)
            REQUIRE(capture.Observe(code[index], index) == ProgramCaptureState::Result::Continue);
        REQUIRE(capture.Observe(code[6], 6) == ProgramCaptureState::Result::Complete);
    }
}

TEST_CASE("Ampere static exit capture does not bypass unsupported lowering", "[video_core][ampere]") {
    const auto lowered = Shader::Ampere::LowerToMaxwell(ExitHandlerProgram);
    REQUIRE_FALSE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.empty());
    REQUIRE(lowered.error_offset == 0);
}

TEST_CASE("Ampere static exit capture validates extended instruction modes", "[video_core][ampere]") {
    for (u32 setup = 0; setup < 5; ++setup) {
        for (u32 bit : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 30U, 31U}) {
            auto code = ExitHandlerProgram;
            code[setup].words[3] ^= 1U << bit;
            ProgramCaptureState capture;
            for (u32 index = 0; index < 6; ++index)
                REQUIRE(capture.Observe(code[index], index) == ProgramCaptureState::Result::Continue);
            REQUIRE(capture.Observe(code[6], 6) == ProgramCaptureState::Result::Complete);
        }
    }
}

TEST_CASE("Ampere static exit capture ignores scheduling fields", "[video_core][ampere]") {
    for (u32 setup = 0; setup < 5; ++setup) {
        for (u32 bit = 9; bit < 30; ++bit) {
            auto code = ExitHandlerProgram;
            code[setup].words[3] ^= 1U << bit;
            ProgramCaptureState capture;
            for (u32 index = 0; index < code.size(); ++index) {
                const auto expected = index + 1 == code.size()
                                          ? ProgramCaptureState::Result::Complete
                                          : ProgramCaptureState::Result::Continue;
                REQUIRE(capture.Observe(code[index], index) == expected);
            }
        }
    }
}

TEST_CASE("Ampere geometry exit capture ends at the handler behind a body call",
          "[video_core][ampere][kirby]") {
    std::array<Instruction, 19> code{};
    std::copy_n(ExitHandlerProgram.begin(), 5, code.begin());
    code[1].words[1] = 0xe0;
    code[5] = Instruction{{0x00007944, 0x10, 0x03c00000, 0x000fea00}};
    code[6] = ExitHandlerProgram[6];
    code[7] = Instruction{{0x08007950, 0xffffff80, 0x03c3ffff, 0x000fea00}};
    code[8] = ExitHandlerProgram[6];
    for (u32 index = 9; index < 14; ++index)
        code[index] = Instruction{{0x00007918, 0, 0, 0x000fc000}};
    std::copy_n(ExitHandlerProgram.begin() + 7, 3, code.begin() + 14);
    code[17] = Instruction{{0x00007947, 0xfffffff0, 0x0383ffff, 0x000fc000}};
    code[18] = Instruction{{0x0000734e, 0, 0, 0x000fe400}};
    ProgramCaptureState capture;
    for (u32 index = 0; index < 17; ++index) {
        const auto expected = index == 16 ? ProgramCaptureState::Result::Complete
                                          : ProgramCaptureState::Result::Continue;
        INFO("instruction " << index);
        REQUIRE(capture.Observe(code[index], index) == expected);
    }

    auto brx_code = code;
    brx_code[9] = Instruction{{0x00007949, 0, 0, 0}};
    brx_code[10] = Instruction{{0x00007947, 0x90, 0x03800000, 0}};
    ProgramCaptureState brx_capture;
    for (u32 index = 0; index < 17; ++index) {
        const auto expected = index == 16 ? ProgramCaptureState::Result::Complete
                                          : ProgramCaptureState::Result::Continue;
        INFO("brx instruction " << index);
        REQUIRE(brx_capture.Observe(brx_code[index], index) == expected);
    }
}
