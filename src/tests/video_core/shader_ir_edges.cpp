// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"

namespace {
namespace IR = Shader::IR;
using IR::Opcode;
using IR::Value;

class EdgeEnvironment final : public Shader::Environment {
public:
    u32 expected_handle{0xabc};
    std::map<std::pair<u32, u32>, u32> cbuf_values{{{0, 0}, 0xabc}};

    u64 ReadInstruction(u32) override {
        throw std::logic_error("Unexpected instruction read");
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        const auto it = cbuf_values.find({index, offset});
        REQUIRE(it != cbuf_values.end());
        return it->second;
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        REQUIRE(handle == expected_handle);
        return false;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        return 0;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {1, 1, 1};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

struct ShaderProgram {
    Shader::ObjectPool<IR::Inst> pool{256};
    IR::Block block{pool};
    IR::Block left{pool};
    IR::Block merge{pool};
    IR::Block* insertion_block{&block};
    IR::Program program;
    EdgeEnvironment env;

    ShaderProgram() {
        program.stage = Shader::Stage::Fragment;
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block}};
    }

    Value Emit(Opcode opcode, std::initializer_list<Value> args = {}) {
        insertion_block->AppendNewInst(opcode, args);
        return Value{&insertion_block->back()};
    }

    Value Storage(u32 offset) {
        return Emit(Opcode::LoadStorage32, {Value{0U}, Value{offset}});
    }

    Value Add(Value lhs, Value rhs) {
        return Emit(Opcode::IAdd32, {lhs, rhs});
    }

    IR::Inst& Sample(Value handle) {
        IR::IREmitter ir{*insertion_block};
        IR::TextureInstInfo flags{};
        flags.type.Assign(Shader::TextureType::Color2D);
        return *ir.ImageSampleExplicitLod(
                      handle, ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(0.0f)),
                      ir.Imm32(0.0f), {}, flags)
                    .Inst();
    }

    void RunTexturePass() {
        Shader::HostTranslateInfo host;
        host.ApplyDescriptorLimitPolicy();
        Shader::Optimization::TexturePass(env, program, host);
    }

    const Shader::TextureDescriptor& Descriptor(const IR::Inst& sample) const {
        REQUIRE(sample.GetOpcode() == Opcode::ImageSampleExplicitLod);
        const u32 index{sample.Flags<IR::TextureInstInfo>().descriptor_index};
        REQUIRE(index < program.info.texture_descriptors.size());
        return program.info.texture_descriptors[index];
    }
};

} // namespace

TEST_CASE("BitFieldUExtract folds 32-bit and zero-width edges and rejects out-of-range fields",
          "[video_core][shader]") {
    constexpr u32 input = 0x91e7a53cU;
    ShaderProgram shader;
    const Value full{shader.Emit(Opcode::BitFieldUExtract, {Value{input}, Value{0U}, Value{32U}})};
    const Value empty{
        shader.Emit(Opcode::BitFieldUExtract, {Value{input}, Value{32U}, Value{0U}})};
    Shader::Optimization::ConstantPropagationPass(shader.env, shader.program);

    REQUIRE(full.IsImmediate());
    CHECK(full.U32() == input);
    REQUIRE(empty.IsImmediate());
    CHECK(empty.U32() == 0U);

    constexpr u32 max = (std::numeric_limits<u32>::max)();
    for (const auto& [base, shift, count] : std::array{
             std::tuple{1U, 16U, 17U},
             std::tuple{0U, 32U, 1U},
             std::tuple{0U, 33U, 0U},
             std::tuple{0U, 33U, max},
             std::tuple{0U, max, max},
         }) {
        CAPTURE(base, shift, count);
        ShaderProgram invalid;
        invalid.Emit(Opcode::BitFieldUExtract, {Value{base}, Value{shift}, Value{count}});
        CHECK_THROWS_AS(Shader::Optimization::ConstantPropagationPass(invalid.env, invalid.program),
                        Shader::LogicError);
    }
}

TEST_CASE("Each word of a static 64-bit constant buffer read binds its own texture",
          "[video_core][shader]") {
    const u32 word = GENERATE(0U, 1U);
    const u32 offset = 0x298U + word * 4U;
    ShaderProgram shader;
    shader.env.cbuf_values = {{{0U, 0x298U}, 0x20020bU}, {{0U, 0x29cU}, 0x20034aU}};
    shader.env.expected_handle = shader.env.cbuf_values.at({0U, offset});
    const Value pair{shader.Emit(Opcode::GetCbufU32x2, {Value{0U}, Value{0x298U}})};
    const Value handle{shader.Emit(Opcode::CompositeExtractU32x2, {pair, Value{word}})};
    auto& sample{shader.Sample(handle)};
    shader.RunTexturePass();

    REQUIRE(shader.program.info.texture_descriptors.size() == 1);
    const auto& descriptor = shader.Descriptor(sample);
    CHECK(descriptor.cbuf_index == 0U);
    CHECK(descriptor.cbuf_offset == offset);
    CHECK(descriptor.handle_program == ~0U);
}

TEST_CASE("The high word of a dynamically indexed cbuf read keeps its descriptor range",
          "[video_core][shader]") {
    const u32 word = GENERATE(0U, 1U);
    constexpr u32 base = 16U * Shader::HostTranslateInfo::DEFAULT_DESCRIPTOR_LIMIT - 16U;
    const u32 offset = base + word * 4U;
    ShaderProgram shader;
    shader.env.cbuf_values = {{{0U, base}, 0x20020bU}, {{0U, base + 4U}, 0x20034aU}};
    shader.env.expected_handle = shader.env.cbuf_values.at({0U, offset});
    const Value stride{shader.Emit(Opcode::ShiftLeftLogical32, {shader.Storage(0U), Value{3U}})};
    const Value pair{
        shader.Emit(Opcode::GetCbufU32x2, {Value{0U}, shader.Add(Value{base}, stride)})};
    const Value handle{shader.Emit(Opcode::CompositeExtractU32x2, {pair, Value{word}})};
    auto& sample{shader.Sample(handle)};
    shader.RunTexturePass();

    REQUIRE(shader.program.info.texture_descriptors.size() == 1);
    const auto& descriptor = shader.Descriptor(sample);
    CHECK(descriptor.cbuf_index == 0U);
    CHECK(descriptor.cbuf_offset == offset);
    CHECK(descriptor.count == 2U - word);
    CHECK(descriptor.handle_program == ~0U);
}

TEST_CASE("Phi operands resolve immediate identities before dead code elimination",
          "[video_core][shader]") {
    const bool replace_operand = GENERATE(false, true);
    ShaderProgram shader;
    using Node = IR::AbstractSyntaxNode;
    IR::IREmitter entry{shader.block};
    const IR::U32 lane{entry.LocalInvocationIdX()};
    const IR::U1 cond{entry.ConditionRef(entry.IEqual(lane, entry.Imm32(0U)))};
    const Value identity{shader.Emit(Opcode::Identity, {Value{7U}})};
    shader.insertion_block = &shader.left;
    const Value other{shader.Emit(Opcode::IAdd32, {lane, Value{1U}})};

    shader.block.AddBranch(&shader.left);
    shader.block.AddBranch(&shader.merge);
    shader.left.AddBranch(&shader.merge);
    shader.insertion_block = &shader.merge;
    const Value phi{shader.Emit(Opcode::Phi)};
    phi.Inst()->SetFlags(IR::Type::U32);
    phi.Inst()->AddPhiOperand(&shader.block, replace_operand ? Value{3U} : identity);
    phi.Inst()->AddPhiOperand(&shader.left, other);
    if (replace_operand) {
        phi.Inst()->SetArg(0, identity);
    }

    const auto pre_dce = phi.Inst()->Arg(0);
    REQUIRE(pre_dce.IsImmediate());
    REQUIRE_FALSE(pre_dce.IsIdentity());
    CHECK(pre_dce.U32() == 7U);

    IR::IREmitter merge{shader.merge};
    merge.WriteShared(32, merge.Imm32(0U), IR::U32{phi});

    auto& program = shader.program;
    program.stage = Shader::Stage::Compute;
    program.workgroup_size = {32, 1, 1};
    program.shared_memory_size = 4;
    program.blocks = {&shader.block, &shader.left, &shader.merge};
    program.post_order_blocks = {&shader.merge, &shader.left, &shader.block};
    program.syntax_list = {
        {{.block = &shader.block}, Node::Type::Block},
        {{.if_node{cond, &shader.left, &shader.merge}}, Node::Type::If},
        {{.block = &shader.left}, Node::Type::Block},
        {{.end_if{&shader.merge}}, Node::Type::EndIf},
        {{.block = &shader.merge}, Node::Type::Block},
        {{}, Node::Type::Return},
    };
    Shader::Optimization::DeadCodeEliminationPass(program);

    const auto first = phi.Inst()->Arg(0);
    REQUIRE(first.IsImmediate());
    REQUIRE_FALSE(first.IsIdentity());
    CHECK(first.U32() == 7U);
    CHECK(phi.Inst()->Arg(1).Inst() == other.Inst());
    CHECK(other.Inst()->UseCount() == 1);

    Shader::Optimization::CollectShaderInfoPass(shader.env, program);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010500;
    const auto spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
    u32 u32_type_id{};
    std::vector<u32> seven_ids;
    std::array<u32, 2> phi_values{};
    size_t phi_count{};
    for (size_t i = 5; i < spirv.size();) {
        const size_t size = spirv[i] >> 16;
        REQUIRE(size != 0);
        REQUIRE(i + size <= spirv.size());
        switch (static_cast<spv::Op>(spirv[i] & 0xffffU)) {
        case spv::Op::OpTypeInt:
            if (size == 4 && spirv[i + 2] == 32 && spirv[i + 3] == 0) {
                u32_type_id = spirv[i + 1];
            }
            break;
        case spv::Op::OpConstant:
            if (size == 4 && spirv[i + 1] == u32_type_id && spirv[i + 3] == 7U) {
                seven_ids.push_back(spirv[i + 2]);
            }
            break;
        case spv::Op::OpPhi:
            REQUIRE(size == 7);
            phi_values = {spirv[i + 3], spirv[i + 5]};
            ++phi_count;
            break;
        default:
            break;
        }
        i += size;
    }
    REQUIRE(u32_type_id != 0);
    REQUIRE_FALSE(seven_ids.empty());
    REQUIRE(phi_count == 1);
    CHECK(phi_values[0] != 0);
    CHECK(phi_values[1] != 0);
    CHECK((std::ranges::find(seven_ids, phi_values[0]) != seven_ids.end() ||
           std::ranges::find(seven_ids, phi_values[1]) != seven_ids.end()));
}

TEST_CASE("PatchPhiNodes throws LogicError when a phi operand has no SPIR-V definition",
          "[video_core][shader]") {
    ShaderProgram shader;
    using Node = IR::AbstractSyntaxNode;
    IR::IREmitter entry{shader.block};
    const IR::U32 lane{entry.LocalInvocationIdX()};
    const IR::U1 cond{entry.ConditionRef(entry.IEqual(lane, entry.Imm32(0U)))};
    shader.insertion_block = &shader.left;
    // Emitted into `left`, which is intentionally omitted from syntax_list so `unemitted` has Id{}.
    const Value unemitted{shader.Emit(Opcode::IAdd32, {lane, Value{1U}})};

    shader.block.AddBranch(&shader.left);
    shader.block.AddBranch(&shader.merge);
    shader.left.AddBranch(&shader.merge);
    shader.insertion_block = &shader.merge;
    const Value phi{shader.Emit(Opcode::Phi)};
    phi.Inst()->SetFlags(IR::Type::U32);
    phi.Inst()->AddPhiOperand(&shader.block, Value{7U});
    phi.Inst()->AddPhiOperand(&shader.left, unemitted);
    IR::IREmitter merge{shader.merge};
    merge.WriteShared(32, merge.Imm32(0U), IR::U32{phi});

    auto& program = shader.program;
    program.stage = Shader::Stage::Compute;
    program.workgroup_size = {32, 1, 1};
    program.shared_memory_size = 4;
    program.blocks = {&shader.block, &shader.left, &shader.merge};
    program.post_order_blocks = {&shader.merge, &shader.left, &shader.block};
    program.syntax_list = {
        {{.block = &shader.block}, Node::Type::Block},
        {{.if_node{cond, &shader.left, &shader.merge}}, Node::Type::If},
        {{.end_if{&shader.merge}}, Node::Type::EndIf},
        {{.block = &shader.merge}, Node::Type::Block},
        {{}, Node::Type::Return},
    };
    Shader::Optimization::CollectShaderInfoPass(shader.env, program);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010500;
    CHECK_THROWS_AS(Shader::Backend::SPIRV::EmitSPIRV(profile, program), Shader::LogicError);
}
