// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_override.h"
#include "video_core/shader_environment.h"

#ifdef EGLANTINE_ENABLE_RUST_AMP_SPIRV
#include "eglantine_ampere_spirv.h"
#endif

namespace {

constexpr u32 kSpirvMagic = 0x07230203U;
constexpr u32 kSpirvVersion16 = 0x00010600U;
constexpr u16 kOpCapability = 17U;
constexpr u16 kOpExtension = 10U;
constexpr u16 kOpMemoryModel = 14U;
constexpr u16 kOpEntryPoint = 15U;
constexpr u16 kOpExecutionMode = 16U;
constexpr u16 kOpSelect = 169U;
constexpr u16 kOpControlBarrier = 224U;
constexpr u16 kOpTypeCooperativeMatrixKHR = 4456U;
constexpr u16 kOpCooperativeMatrixMulAddKHR = 4459U;
constexpr u32 kCapCooperativeMatrixKHR = 6022U;

constexpr Shader::Ampere::Instruction MakeSm86(u32 w0, u32 w1, u32 w2, u32 w3 = 0U) {
    return Shader::Ampere::Instruction{{w0, w1, w2, w3}};
}

constexpr Shader::Ampere::Instruction Sm86Exit() {
    return MakeSm86(0x0000794dU, 0U, 0U);
}

constexpr Shader::Ampere::Instruction Sm86S2r(u32 dst, u32 special) {
    return MakeSm86(0x919U | (7U << 12) | ((dst & 0xffU) << 16), 0U, special << 8);
}

constexpr Shader::Ampere::Instruction Sm86MovImm(u32 dst, u32 imm) {
    return MakeSm86(0x802U | (7U << 12) | ((dst & 0xffU) << 16), imm, 0U);
}

constexpr Shader::Ampere::Instruction Sm86MovCbuf(u32 dst, u32 bank, u32 byte_off) {
    return MakeSm86(0xa02U | (7U << 12) | ((dst & 0xffU) << 16), (bank << 22) | (byte_off << 6), 0U);
}

constexpr Shader::Ampere::Instruction Sm86UldcU64(u32 dst_ur, u32 bank, u32 byte_off) {
    return MakeSm86(0xab9U | (7U << 12) | ((dst_ur & 0x3fU) << 16), (bank << 22) | (byte_off << 6),
                    5U << 9);
}

constexpr Shader::Ampere::Instruction Sm86ShfLImm(u32 dst, u32 src_a, u32 shift) {
    return MakeSm86(0x819U | (7U << 12) | ((dst & 0xffU) << 16) | ((src_a & 0xffU) << 24), shift,
                    255U | (3U << 9));
}

constexpr Shader::Ampere::Instruction Sm86LdgUr(u32 dst, u32 base_reg, u32 ur, u32 imm_off = 0U) {
    return MakeSm86(0x981U | (7U << 12) | ((dst & 0xffU) << 16) | ((base_reg & 0xffU) << 24),
                    ur | ((imm_off & 0xff'ffffU) << 8), 0x081e8100U | (4U << 9));
}

constexpr Shader::Ampere::Instruction Sm86StgUr(u32 src_reg, u32 base_reg, u32 ur,
                                                u32 imm_off = 0U) {
    return MakeSm86(0x986U | (7U << 12) | ((base_reg & 0xffU) << 24),
                    src_reg | ((imm_off & 0xff'ffffU) << 8), 0x0810a100U | ur | (4U << 9));
}

constexpr Shader::Ampere::Instruction Sm86StsB32(u32 src_reg, u32 base_reg, u32 imm_off = 0U) {
    return MakeSm86(0x388U | (7U << 12) | ((base_reg & 0xffU) << 24),
                    src_reg | ((imm_off & 0xff'ffffU) << 8), 4U << 9);
}

constexpr Shader::Ampere::Instruction Sm86LdsB32(u32 dst, u32 base_reg, u32 imm_off = 0U) {
    return MakeSm86(0x984U | (7U << 12) | ((dst & 0xffU) << 16) | ((base_reg & 0xffU) << 24),
                    (imm_off & 0xff'ffffU) << 8, 4U << 9);
}

constexpr Shader::Ampere::Instruction Sm86FaddReg(u32 dst, u32 src_a, u32 src_b) {
    return MakeSm86(0x221U | (7U << 12) | ((dst & 0xffU) << 16) | ((src_a & 0xffU) << 24), src_b,
                    0U);
}

constexpr Shader::Ampere::Instruction Sm86BarSync() {
    return MakeSm86(0xb1dU | (7U << 12), 0U, 0U);
}

constexpr Shader::Ampere::Instruction Sm86Hmma16816(u32 dst, u32 ra, u32 rb, u32 rc) {
    return MakeSm86(0x23cU | (7U << 12) | ((dst & 0xffU) << 16) | ((ra & 0xffU) << 24), rb,
                    rc | 0x800U);
}

#ifdef EGLANTINE_ENABLE_RUST_AMP_SPIRV
struct ParsedSpirvOp {
    u16 opcode{};
    std::vector<u32> operands;
};

std::vector<ParsedSpirvOp> ParseAndVerifySpirv16(const u32* words, size_t count, u32 expected_wg_x) {
    REQUIRE(words != nullptr);
    REQUIRE(count > 5U);
    CHECK(words[0] == kSpirvMagic);
    CHECK(words[1] == kSpirvVersion16);
    CHECK(words[3] > 1U);
    CHECK(words[4] == 0U);

    const u32 bound = words[3];
    std::vector<ParsedSpirvOp> parsed;
    size_t idx = 5U;
    bool saw_memory_model = false;
    bool saw_entry_point = false;
    bool saw_local_size = false;

    while (idx < count) {
        const u32 hdr = words[idx];
        const size_t wc = hdr >> 16;
        const u16 opcode = static_cast<u16>(hdr & 0xffffU);
        REQUIRE(wc >= 1U);
        REQUIRE(idx + wc <= count);
        ParsedSpirvOp op;
        op.opcode = opcode;
        op.operands.assign(words + idx + 1, words + idx + wc);
        if (opcode == kOpMemoryModel) {
            REQUIRE(op.operands.size() == 2U);
            CHECK(op.operands[0] == 0U); // Logical
            CHECK(op.operands[1] == 3U); // Vulkan
            saw_memory_model = true;
        } else if (opcode == kOpEntryPoint) {
            REQUIRE(op.operands.size() >= 3U);
            CHECK(op.operands[0] == 5U); // GLCompute
            saw_entry_point = true;
        } else if (opcode == kOpExecutionMode && op.operands.size() >= 5U &&
                   op.operands[1] == 17U) {
            CHECK(op.operands[2] == expected_wg_x);
            saw_local_size = true;
        }
        for (const u32 word : op.operands) {
            (void)bound;
            (void)word;
        }
        parsed.push_back(std::move(op));
        idx += wc;
    }
    REQUIRE(idx == count);
    CHECK(saw_memory_model);
    CHECK(saw_entry_point);
    CHECK(saw_local_size);
    return parsed;
}

EglSpirvProfile MakeSpirv16Profile(bool coop_supported, u32 tile_n = 8U,
                                     std::array<u32, 4> a_map = {0U, 2U, 1U, 3U}) {
    EglSpirvProfile profile{};
    profile.supported_spirv = kSpirvVersion16;
    profile.max_subgroup_size = 32U;
    profile.supported_subgroup_stages = 0xffff'ffffU;
    profile.max_user_clip_distances = 8U;
    profile.min_ssbo_alignment = 16U;
    profile.unified_descriptor_binding = 1U;
    profile.support_int8 = 1U;
    profile.support_int16 = 1U;
    profile.support_int64 = 1U;
    profile.support_float16 = 1U;
    profile.support_float_controls = 1U;
    profile.support_vote = 1U;
    profile.support_shuffle_relative = 1U;
    profile.coop_layout.supported = coop_supported ? 1U : 0U;
    profile.coop_layout.use_fp16_accum = 1U;
    profile.coop_layout.tile_m = 16U;
    profile.coop_layout.tile_n = tile_n;
    profile.coop_layout.tile_k = 16U;
    profile.coop_layout.subgroup_size = 32U;
    profile.coop_layout.a_word_map[0] = a_map[0];
    profile.coop_layout.a_word_map[1] = a_map[1];
    profile.coop_layout.a_word_map[2] = a_map[2];
    profile.coop_layout.a_word_map[3] = a_map[3];
    profile.coop_layout.b_word_map[0] = 0U;
    profile.coop_layout.b_word_map[1] = 1U;
    profile.coop_layout.c_word_map[0] = 0U;
    profile.coop_layout.c_word_map[1] = 1U;
    profile.coop_layout.d_word_map[0] = 0U;
    profile.coop_layout.d_word_map[1] = 1U;
    return profile;
}
#endif

} // namespace

TEST_CASE("NativeShaderSnapshot captures immutable SM86 state", "[video_core][ampere]") {
    VideoCommon::NativeShaderSnapshot snap;
    snap.stage = Shader::Stage::Compute;
    snap.workgroup_size = {64U, 1U, 1U};
    snap.shared_memory_size = 256U;
    snap.native_code = {Sm86MovImm(2U, 42U), Sm86Exit()};
    snap.program_key = Shader::Ampere::NativeProgramKey(snap.native_code);
    snap.cbuf_values.emplace((static_cast<u64>(0U) << 32) | 0x20U, 0x3f800000U);
    snap.storage_pointer_types.emplace((static_cast<u64>(0U) << 32) | 0x160U, true);

    const VideoCommon::NativeShaderSnapshot copy = snap;
    CHECK(copy.stage == Shader::Stage::Compute);
    CHECK(copy.workgroup_size[0] == 64U);
    CHECK(copy.shared_memory_size == 256U);
    CHECK(copy.native_code.size() == 2U);
    CHECK(copy.program_key != 0ULL);
    CHECK(copy.cbuf_values.size() == 1U);
    CHECK(copy.storage_pointer_types.at((static_cast<u64>(0U) << 32) | 0x160U));
}

#ifdef EGLANTINE_ENABLE_RUST_AMP_SPIRV
TEST_CASE("Direct SM86 compute kernel compiles to valid SPIR-V 1.6", "[video_core][ampere]") {
    const std::vector<Shader::Ampere::Instruction> code = {
        Sm86UldcU64(4U, 0U, 0x160U),
        Sm86UldcU64(6U, 0U, 0x168U),
        Sm86S2r(2U, 33U),
        Sm86MovCbuf(3U, 24U, 0x20U),
        Sm86ShfLImm(4U, 2U, 2U),
        Sm86LdgUr(5U, 4U, 4U),
        Sm86StsB32(5U, 4U),
        Sm86BarSync(),
        Sm86LdsB32(7U, 4U),
        Sm86FaddReg(8U, 7U, 3U),
        Sm86StgUr(8U, 4U, 6U),
        Sm86Exit(),
    };
    const std::vector<EglAmpereInstruction> ffi_code = [&] {
        std::vector<EglAmpereInstruction> out;
        out.reserve(code.size());
        for (const auto& insn : code) {
            EglAmpereInstruction c{};
            std::memcpy(c.words, insn.words.data(), sizeof(c.words));
            out.push_back(c);
        }
        return out;
    }();
    const std::array<EglSpirvStoragePtrEntry, 2> ptrs{{
        {.key = (static_cast<u64>(0U) << 32) | 0x160U, .is_mapped = 1U},
        {.key = (static_cast<u64>(0U) << 32) | 0x168U, .is_mapped = 1U},
    }};

    EglSpirvCompileInput input{};
    input.stage = EGL_SPIRV_STAGE_COMPUTE;
    input.texture_bound = ~0U;
    input.program_key = Shader::Ampere::NativeProgramKey(code);
    input.code = ffi_code.data();
    input.code_count = ffi_code.size();
    input.workgroup_size[0] = 64U;
    input.workgroup_size[1] = 1U;
    input.workgroup_size[2] = 1U;
    input.shared_memory_size = 256U;
    input.storage_ptrs = ptrs.data();
    input.storage_ptr_count = ptrs.size();
    input.profile = MakeSpirv16Profile(true);

    EglSpirvCompileOutput output{};
    REQUIRE(egl_ampere_spirv_compile(&input, &output) == 0);
    CHECK(output.fallback_to_maxwell == 0U);
    CHECK(output.error == nullptr);
    CHECK(output.error_len == 0U);

    const auto ops = ParseAndVerifySpirv16(output.spirv_words, output.word_count, 64U);
    const bool has_barrier = std::any_of(ops.begin(), ops.end(), [](const ParsedSpirvOp& op) {
        return op.opcode == kOpControlBarrier;
    });
    CHECK(has_barrier);
    CHECK(output.info.uses_local_invocation_id == 1U);
    CHECK(output.info.uses_global_memory == 1U);
    CHECK(output.info.stores_global_memory == 1U);
    CHECK((output.info.constant_buffer_mask & 1U) != 0U);
    CHECK((output.info.constant_buffer_mask & (1U << 17U)) != 0U);
    CHECK(output.info.storage_buffer_descriptor_count == 2U);

    egl_ampere_spirv_free(&output);
    CHECK(output.spirv_words == nullptr);
    CHECK(output.word_count == 0U);
    egl_ampere_spirv_free(&output);
}

TEST_CASE("Direct SM86 HMMA emits parameterized cooperative matrix without OpSelect hack",
          "[video_core][ampere]") {
    const std::vector<EglAmpereInstruction> ffi_code = {
        [] {
            const auto i = Sm86UldcU64(4U, 0U, 0x160U);
            return EglAmpereInstruction{{i.words[0], i.words[1], i.words[2], i.words[3]}};
        }(),
        [] {
            const auto i = Sm86MovImm(8U, 0x3c003c00U);
            return EglAmpereInstruction{{i.words[0], i.words[1], i.words[2], i.words[3]}};
        }(),
        [] {
            const auto i = Sm86Hmma16816(16U, 8U, 12U, 255U);
            return EglAmpereInstruction{{i.words[0], i.words[1], i.words[2], i.words[3]}};
        }(),
        [] {
            const auto i = Sm86StgUr(16U, 255U, 4U);
            return EglAmpereInstruction{{i.words[0], i.words[1], i.words[2], i.words[3]}};
        }(),
        [] {
            const auto i = Sm86Exit();
            return EglAmpereInstruction{{i.words[0], i.words[1], i.words[2], i.words[3]}};
        }(),
    };

    auto compile_with = [&](u32 tile_n, std::array<u32, 4> a_map) {
        EglSpirvCompileInput in{};
        in.stage = EGL_SPIRV_STAGE_COMPUTE;
        in.texture_bound = ~0U;
        in.program_key = 0xcafe0001ULL;
        in.code = ffi_code.data();
        in.code_count = ffi_code.size();
        in.workgroup_size[0] = 32U;
        in.workgroup_size[1] = 1U;
        in.workgroup_size[2] = 1U;
        in.profile = MakeSpirv16Profile(true, tile_n, a_map);

        EglSpirvCompileOutput out{};
        REQUIRE(egl_ampere_spirv_compile(&in, &out) == 0);
        CHECK(out.fallback_to_maxwell == 0U);
        CHECK(out.info.uses_cooperative_matrix == 1U);
        CHECK(out.info.requires_full_subgroups == 1U);

        std::vector<u32> words(out.spirv_words, out.spirv_words + out.word_count);
        egl_ampere_spirv_free(&out);
        return words;
    };

    const auto words_nv = compile_with(8U, {0U, 2U, 1U, 3U});
    const auto words_custom = compile_with(16U, {3U, 1U, 2U, 0U});
    CHECK(words_nv != words_custom);

    const auto ops = ParseAndVerifySpirv16(words_nv.data(), words_nv.size(), 32U);
    CHECK(std::any_of(ops.begin(), ops.end(), [](const ParsedSpirvOp& op) {
        return op.opcode == kOpCapability && !op.operands.empty() &&
               op.operands[0] == kCapCooperativeMatrixKHR;
    }));
    CHECK(std::any_of(ops.begin(), ops.end(), [](const ParsedSpirvOp& op) {
        return op.opcode == kOpTypeCooperativeMatrixKHR;
    }));
    CHECK(std::any_of(ops.begin(), ops.end(), [](const ParsedSpirvOp& op) {
        return op.opcode == kOpCooperativeMatrixMulAddKHR;
    }));
    CHECK(std::any_of(ops.begin(), ops.end(), [](const ParsedSpirvOp& op) {
        if (op.opcode != kOpExtension) {
            return false;
        }
        std::string ext;
        for (const u32 w : op.operands) {
            for (u32 b = 0U; b < 4U; ++b) {
                const char ch = static_cast<char>((w >> (b * 8U)) & 0xffU);
                if (ch == '\0') {
                    return ext == "SPV_KHR_cooperative_matrix";
                }
                ext.push_back(ch);
            }
        }
        return false;
    }));
    CHECK(std::none_of(ops.begin(), ops.end(), [](const ParsedSpirvOp& op) {
        return op.opcode == kOpSelect;
    }));
}

TEST_CASE("Direct SM86 batch compilation and fallback to Maxwell", "[video_core][ampere]") {
    const std::array<EglAmpereInstruction, 4> valid_code{{
        {{0xab9U | (7U << 12) | (4U << 16), 0x160U << 6, 5U << 9, 0U}},
        {{0x802U | (7U << 12) | (2U << 16), 77U, 0U, 0U}},
        {{0x986U | (7U << 12) | (255U << 24), 2U, 0x0810a100U | 4U | (4U << 9), 0U}},
        {{0x94dU | (7U << 12), 0U, 0U, 0U}},
    }};
    const std::array<EglAmpereInstruction, 1> unknown_code{{
        {{0x00007b60U, 0U, 0U, 0U}},
    }};

    EglSpirvCompileInput in_valid{};
    in_valid.stage = EGL_SPIRV_STAGE_COMPUTE;
    in_valid.texture_bound = ~0U;
    in_valid.code = valid_code.data();
    in_valid.code_count = valid_code.size();
    in_valid.workgroup_size[0] = 32U;
    in_valid.workgroup_size[1] = 1U;
    in_valid.workgroup_size[2] = 1U;
    in_valid.profile = MakeSpirv16Profile(true);

    EglSpirvCompileInput in_fallback = in_valid;
    in_fallback.code = unknown_code.data();
    in_fallback.code_count = unknown_code.size();

    std::array<EglSpirvCompileInput, 4> inputs{in_valid, in_fallback, in_valid, in_fallback};
    std::array<EglSpirvCompileOutput, 4> outputs{};
    REQUIRE(egl_ampere_spirv_compile_batch(inputs.data(), inputs.size(), outputs.data()) == 0);

    for (size_t i = 0; i < outputs.size(); ++i) {
        if (i % 2U == 0U) {
            CHECK(outputs[i].fallback_to_maxwell == 0U);
            CHECK(outputs[i].word_count > 5U);
        } else {
            CHECK(outputs[i].fallback_to_maxwell == 1U);
            CHECK(outputs[i].fallback_opcode == 0xb60U);
            CHECK(outputs[i].error == nullptr);
            CHECK(outputs[i].error_len == 0U);
        }
        egl_ampere_spirv_free(&outputs[i]);
    }
}
#endif
