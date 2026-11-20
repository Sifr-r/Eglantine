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
using Shader::Ampere::ProgramCaptureState;

constexpr Instruction EXIT{{0x0000794d, 0, 0x03800000, 0}};

// Lowered Maxwell words whose top bits (above `shift`) are `opcode`.
template <typename Program>
size_t CountMaxwell(const Program& lowered, u64 opcode, u32 shift) {
    return static_cast<size_t>(std::count_if(lowered.instructions.begin(),
                                             lowered.instructions.end(),
                                             [&](u64 word) { return (word >> shift) == opcode; }));
}

template <typename Program>
u64 FindMaxwell(const Program& lowered, u64 opcode, u32 shift) {
    const auto it = std::find_if(lowered.instructions.begin(), lowered.instructions.end(),
                                 [&](u64 word) { return (word >> shift) == opcode; });
    return it != lowered.instructions.end() ? *it : 0;
}

TEST_CASE("Ampere BRA.CONV always branches and BRA.DIV never does", "[video_core][ampere]") {
    // BRA.CONV UR6 / BRA.DIV ~URZ to the second EXIT: the mode is in the offset's low two bits.
    const std::array<Instruction, 3> convergent{{
        {{0x06007947, 0x10 | 3, 0x0b800000, 0}},
        EXIT,
        EXIT,
    }};
    ProgramCaptureState capture;
    REQUIRE(capture.Observe(convergent[0], 0) == ProgramCaptureState::Result::Continue);
    const auto taken = LowerToMaxwell(convergent);
    REQUIRE(static_cast<bool>(taken));
    REQUIRE(CountMaxwell(taken, 0xe24, 52) == 1);

    const std::array<Instruction, 3> divergent{{
        {{0xff007947, 0x10 | 2, 0x0b800000, 0}},
        EXIT,
        EXIT,
    }};
    const auto dropped = LowerToMaxwell(divergent);
    REQUIRE(static_cast<bool>(dropped));
    REQUIRE(CountMaxwell(dropped, 0xe24, 52) == 0);
}

TEST_CASE("Ampere TLD.SCR.LZ takes its handle from a uniform register", "[video_core][ampere]") {
    // TLD.SCR.LZ R5, R8, R4, R5, UR4, 0x0, 2D, 0x7 -> Maxwell TLD.B.
    const std::array<Instruction, 2> code{{
        {{0x04087f66, 0x30000405, 0x089e0705, 0x001f4200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u64 tld = FindMaxwell(lowered, 0xdd, 56);
    REQUIRE(tld != 0);
    REQUIRE(((tld >> 28) & 7) == 2); // 2D
    REQUIRE(((tld >> 31) & 0xf) == 7);
}

TEST_CASE("Ampere I2F converts signed 16-bit sources", "[video_core][ampere]") {
    // I2F.S16 R8, R8
    const std::array<Instruction, 2> code{{
        {{0x00087306, 0x00000008, 0x00101400, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u64 i2f = FindMaxwell(lowered, 0x5cb, 52);
    REQUIRE(((i2f >> 10) & 3) == 1); // 16-bit source
    REQUIRE(((i2f >> 13) & 1) == 1); // signed
}

TEST_CASE("Ampere integer-to-float conversions take immediates", "[video_core][ampere]") {
    // I2F.U32.RP R7, 0x20; I2FP.F32.U32.RZ R4, 0x100; WARPSYNC R7
    const std::array<Instruction, 4> code{{
        {{0x00077906, 0x00000020, 0x00209000, 0x000e2200}},
        {{0x00047845, 0x00000100, 0x0020d000, 0x000fe200}},
        {{0x00007348, 0x00000007, 0x03800000, 0x000fe800}},
        EXIT,
    }};
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));
}

TEST_CASE("Ampere SUST.P keeps its channel mask", "[video_core][ampere]") {
    // SUST.P.2D.STRONG.SM.R.IGN [R6], R4, 0x0, 0x59
    const std::array<Instruction, 2> code{{
        {{0x06007b9b, 0x60005904, 0x0010a100, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(((FindMaxwell(lowered, 0xeb2, 52) >> 20) & 0xf) == 1);
}

TEST_CASE("Ampere SHFL with a register lane keeps its mode", "[video_core][ampere]") {
    // SHFL.DOWN PT, R7, R0, R5, 0x1f
    const std::array<Instruction, 2> code{{
        {{0x00077589, 0x08001f05, 0x000e0000, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(((FindMaxwell(lowered, 0xef1, 52) >> 30) & 3) == 2);
}

TEST_CASE("Ampere SHR preserves the guard and the clamp mode", "[video_core][ampere]") {
    constexpr u32 Destination = 11;
    constexpr u32 Source = 19;
    constexpr u32 NegatedP3 = 8 | 3;
    constexpr u32 ShiftRegister = 249;
    constexpr u32 U32RightShift = (3 << 9) | (1 << 12);
    for (const bool wrap : {false, true}) {
        CAPTURE(wrap);
        // SHF.R.U32 R11, R19, R249, RZ, testing both values of the wrap bit.
        const Instruction shift{{0x219 | (NegatedP3 << 12) | (Destination << 16) | (Source << 24),
                                 ShiftRegister, U32RightShift | (u32{wrap} << 11) | 255, 0}};
        const auto lowered = LowerToMaxwell(std::array{shift, EXIT});
        INFO(lowered.error);
        REQUIRE(lowered);
        const auto shr = std::ranges::find_if(lowered.instructions, [](u64 word) {
            return (word >> 48) == 0x5c28;
        });
        REQUIRE(shr != lowered.instructions.end());
        CHECK((*shr & 255) == Destination);
        CHECK(((*shr >> 8) & 255) == Source);
        CHECK(((*shr >> 16) & 15) == NegatedP3);
        CHECK(((*shr >> 39) & 1) == u64{wrap});

        // The native shift amount lies in the reserved range and must come from bank 1.
        const u32 amount = static_cast<u32>((*shr >> 20) & 255);
        CHECK(amount < 244);
        const u64 load_amount = Shader::Ampere::NativeRegisterCopy(0, amount, 1, ShiftRegister);
        CHECK(std::find(lowered.instructions.begin(), shr, load_amount) != shr);
    }
}

TEST_CASE("Ampere calls find a return address set ahead of them", "[video_core][ampere]") {
    // MOV R9, 0x30; MOV R4, 0x5; CALL.REL.NOINC; EXIT; RET.REL.NODEC R9
    std::array<Instruction, 5> code{{
        {{0x00097802, 0x30, 0x00000f00, 0}},
        {{0x00047802, 0x5, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        EXIT,
        {{0x09007950, 0xffffffb0, 0x03c3ffff, 0}},
    }};
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));

    // The link register is written again between its MOV and the call.
    code[1] = {{0x00097802, 0x5, 0x00000f00, 0}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(!static_cast<bool>(lowered));
    REQUIRE(lowered.error == "native call linkage mode");
}

TEST_CASE("Ampere programs go on past an EXIT that a branch jumps over", "[video_core][ampere]") {
    // A call, then a branch over an EXIT to more of the program, with another call there.
    const std::array<Instruction, 8> code{{
        {{0x00047802, 0x20, 0x00000f00, 0}},
        {{0x00007944, 0x50, 0x03c00000, 0}},
        {{0x00007947, 0x10, 0x03800000, 0}},
        EXIT,
        {{0x00047802, 0x60, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        EXIT,
        {{0x04007950, 0xffffff80, 0x03c3ffff, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.source_size == sizeof(code));
}

TEST_CASE("Ampere capture continues over a branch-skipped EXIT", "[video_core][ampere]") {
    ProgramCaptureState jump;
    CHECK(jump.Observe({{0x00007944, 0x10, 0x03c00000, 0}}, 0) == ProgramCaptureState::Result::Continue);
    CHECK(jump.Observe(EXIT, 1) == ProgramCaptureState::Result::Continue);
    CHECK(jump.Observe(EXIT, 2) == ProgramCaptureState::Result::Complete);
}

TEST_CASE("Ampere SEL accepts a uniform source", "[video_core][ampere]") {
    // SEL R3, R4, UR7, P0
    const std::array<Instruction, 2> select{{
        {{0x04037c07, 0x7, 0x08000000, 0x000fe400}},
        EXIT,
    }};
    const auto selected = LowerToMaxwell(select);
    INFO(selected.error);
    REQUIRE(static_cast<bool>(selected));
    CHECK(selected.instructions.front() == 0x5ca000000df70403ULL);
}

TEST_CASE("Ampere uniform clamps fold only for matching predicates", "[video_core][ampere]") {
    // UISETP.LT.U32.AND UPc, UPT, UR4, 0x2, UPT + USEL UR4, UR4, 0x2, UPc:
    // Matching UP0/UP1/UP2 clamps fold USEL to IMNMX.U32 while preserving UISETP R239 write.
    for (const u32 cond : {0U, 1U, 2U}) {
        const auto clamped = LowerToMaxwell(std::array<Instruction, 3>{{
            {{0x0400788c, 0x00000002, 0x0bf01070U | (cond << 17), 0x000fc800}},
            {{0x04047887, 0x00000002, 0x08000000U | (cond << 23), 0x000fc800}},
            EXIT,
        }});
        REQUIRE(static_cast<bool>(clamped));
        CHECK(CountMaxwell(clamped, 0x382003800027dcdcULL, 0) == 1);
        CHECK(CountMaxwell(
            clamped, 0x38e800000007efefULL | (u64{1U << (6U - cond)} << 20), 0) == 1);
    }
    // Mismatched predicate (UP1 vs UP2), negated USEL (!UP1), or guarded UISETP/USEL must not fold.
    for (const auto& non_clamp : {
             std::array<Instruction, 3>{{
                 {{0x0400788c, 0x00000002, 0x0bf21070, 0x000fc800}},
                 {{0x04047887, 0x00000002, 0x09000000, 0x000fc800}}, EXIT}},
             std::array<Instruction, 3>{{
                 {{0x0400788c, 0x00000002, 0x0bf21070, 0x000fc800}},
                 {{0x04047887, 0x00000002, 0x0c800000, 0x000fc800}}, EXIT}},
             std::array<Instruction, 3>{{
                 {{0x0400088c, 0x00000002, 0x0bf21070, 0x000fc800}},
                 {{0x04047887, 0x00000002, 0x08800000, 0x000fc800}}, EXIT}},
             std::array<Instruction, 3>{{
                 {{0x0400788c, 0x00000002, 0x0bf21070, 0x000fc800}},
                 {{0x04040887, 0x00000002, 0x08800000, 0x000fc800}}, EXIT}},
         }) {
        const auto res = LowerToMaxwell(non_clamp);
        REQUIRE(static_cast<bool>(res));
        CHECK(CountMaxwell(res, 0x382003800027dcdcULL, 0) == 0);
    }
}

TEST_CASE("Ampere three-input IADD3 retains register source negation", "[video_core][ampere]") {
    const std::array<Instruction, 2> code{{
        {{0x04047210, 0x80000005, 0x07ffe907, 0x000fc400}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    std::vector<u64> adds;
    for (const u64 word : lowered.instructions) {
        if ((word >> 52) == 0x5c1) adds.push_back(word);
    }
    REQUIRE(adds.size() == 2);
    CHECK(((adds[0] >> 48) & 1) == 1);
    CHECK(((adds[0] >> 49) & 1) == 1);
    CHECK(((adds[1] >> 48) & 1) == 1);
}

TEST_CASE("Ampere SHF.R.U64 immediate shifts emit unclamped SHF.R", "[video_core][ampere]") {
    for (const u32 shift : {1U, 12U, 32U}) {
        CAPTURE(shift);
        const Instruction insn{{0x819U | (7U << 12) | (4U << 16) | (6U << 24), shift, 0x1200U | 8U, 0}};
        const auto lowered = LowerToMaxwell(std::array{insn, EXIT});
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        const u64 shf = FindMaxwell(lowered, 0x38f8, 48);
        REQUIRE(shf != 0);
        CHECK((shf & 255) == 4);
        CHECK(((shf >> 8) & 255) == 6);
        CHECK(((shf >> 20) & 0x7ffff) == shift);
        CHECK(((shf >> 39) & 255) == 8);
    }
    const Instruction too_wide{{0x819U | (7U << 12) | (4U << 16) | (6U << 24), 33U, 0x1200U | 8U, 0}};
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{too_wide, EXIT})));
}

TEST_CASE("Ampere ProgramCaptureState continues past EXIT to capture called helpers",
          "[video_core][ampere]") {
    const std::array<Instruction, 4> program{{
        {{0x00097802, 0x20, 0x00000f00, 0}},       // MOV R9, 0x20 (return PC after CALL at index 1)
        {{0x00007944, 0x10, 0x03c00000, 0}},       // CALL.REL.NOINC -> index 3
        EXIT,                                      // main EXIT before callee
        {{0x09007950, 0xffffffc0, 0x03c3ffff, 0}}, // RET.REL.NODEC R9
    }};
    ProgramCaptureState capture;
    CHECK(capture.Observe(program[0], 0) == ProgramCaptureState::Result::Continue);
    CHECK(capture.Observe(program[1], 1) == ProgramCaptureState::Result::Continue);
    CHECK(capture.Observe(program[2], 2) == ProgramCaptureState::Result::Continue);
    CHECK(capture.Observe(program[3], 3) == ProgramCaptureState::Result::Complete);
    const auto lowered = LowerToMaxwell(program);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
}

TEST_CASE("Ampere USEL, UBREV, UFLO, and UPOPC uniform operations lower cleanly",
          "[video_core][ampere]") {
    // USEL UR4, UR6, UR8, UP1 (0x287) + USEL UR10, UR6, 0x42, UP2 (0x887)
    const Instruction usel_reg{{0x287U | (7U << 12) | (4U << 16) | (6U << 24), 8U, 0x08000000U | (1U << 23), 0}};
    const Instruction usel_imm{{0x887U | (7U << 12) | (10U << 16) | (6U << 24), 0x42U, 0x08000000U | (2U << 23), 0}};
    // UBREV UR12, UR6; UFLO.U32.SH UR14, UR6; UPOPC UR16, ~UR6
    const Instruction ubrev{{0x2beU | (7U << 12) | (12U << 16), 6U, 0x08000000U, 0}};
    const Instruction uflo{{0x2bdU | (7U << 12) | (14U << 16), 6U, 0x080e0400U, 0}};
    const Instruction upopc{{0x2bfU | (7U << 12) | (16U << 16), 6U | (1U << 31), 0x08000000U, 0}};

    const auto lowered = LowerToMaxwell(std::array{usel_reg, usel_imm, ubrev, uflo, upopc, EXIT});
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));

    CHECK(CountMaxwell(lowered, 0x5ca, 52) == 2); // Two SELs (P5 and P4)
    const u64 brev = FindMaxwell(lowered, 0x3828, 48);
    REQUIRE(brev != 0);
    CHECK((brev & 255) == 228); // UR12
    CHECK(((brev >> 8) & 255) == 222); // UR6
    CHECK(((brev >> 39) & 3) == 3);

    const u64 flo = FindMaxwell(lowered, 0x5c30, 48);
    REQUIRE(flo != 0);
    CHECK((flo & 255) == 230); // UR14
    CHECK(((flo >> 20) & 255) == 222);
    CHECK(((flo >> 41) & 1) == 1); // .SH

    const u64 popc = FindMaxwell(lowered, 0x5c08, 48);
    REQUIRE(popc != 0);
    CHECK((popc & 255) == 232); // UR16
    CHECK(((popc >> 20) & 255) == 222);
    CHECK(((popc >> 40) & 1) == 1); // inverted source
}

TEST_CASE("Ampere three-input IADD3 keeps negative immediates unnegated", "[video_core][ampere]") {
    for (const bool negate_c : {false, true}) {
        CAPTURE(negate_c);
        // IADD3 R4, R6, -4 (0xfffffffc), [-]R8: bit 63 is the negative literal's sign bit.
        const Instruction add{{0x810U | (7U << 12) | (4U << 16) | (6U << 24), 0xfffffffcU,
                               0x07ffe008U | (u32{negate_c} << 11), 0}};
        const auto lowered = LowerToMaxwell(std::array{add, EXIT});
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        const u64 mov = FindMaxwell(lowered, 0x01, 56);
        REQUIRE(mov != 0);
        CHECK((mov & 255) == 251);
        CHECK(((mov >> 20) & 0xffffffffU) == 0xfffffffcU);
        std::vector<u64> adds;
        for (const u64 word : lowered.instructions) {
            if ((word >> 52) == 0x5c1) adds.push_back(word);
        }
        REQUIRE(adds.size() == 2);
        CHECK(((adds[0] >> 48) & 1) == 0);
        CHECK(((adds[0] >> 49) & 1) == 0);
        CHECK(((adds[1] >> 48) & 1) == u64{negate_c});
    }
}
} // namespace
