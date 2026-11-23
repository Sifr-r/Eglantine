// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace Shader::Ampere {
class ProgramCaptureState {
public:
    enum class Result { Continue, Complete, InvalidTarget };
    static constexpr u32 MaximumInstructions = 65536;

#ifdef EGLANTINE_ENABLE_RUST_AMPERE
    ProgramCaptureState()
        : use_rust(detail::IsEnvFlagEnabled("EDEN_NX2_RUST_AMPERE")),
          verify_rust(detail::IsEnvFlagEnabled("EDEN_NX2_RUST_AMPERE_VERIFY")) {
        egl_ampere_capture_init(&rs_state);
    }

    Result Observe(const Instruction& instruction, u32 index) {
        if (verify_rust) {
            const Result cpp_res = ObserveCpp(instruction, index);
            const Result rust_res = ObserveRust(instruction, index);
            bool exit_setup_match = true;
            for (size_t i = 0; i < exit_setup.size(); ++i) {
                if (!std::equal(exit_setup[i].words.begin(), exit_setup[i].words.end(),
                                rs_state.exit_setup[i].words)) {
                    exit_setup_match = false;
                    break;
                }
            }
            const bool state_match = exit_setup_match &&
                                     exit_handler_end == rs_state.exit_handler_end &&
                                     last_call_target == rs_state.last_call_target &&
                                     last_forward_target == rs_state.last_forward_target &&
                                     main_exited == (rs_state.main_exited != 0) &&
                                     computed_branch == (rs_state.computed_branch != 0);
            if (cpp_res != rust_res || !state_match) {
                std::fprintf(
                    stderr,
                    "[EDEN_NX2_RUST_AMPERE_VERIFY] ProgramCaptureState mismatch at index %u "
                    "(op=0x%03x): cpp=%d rust=%d state_match=%d\n",
                    static_cast<unsigned>(index), static_cast<unsigned>(instruction.Bits(0, 12)),
                    static_cast<int>(cpp_res), static_cast<int>(rust_res), state_match ? 1 : 0);
                std::fflush(stderr);
                std::abort();
            }
            return cpp_res;
        }
        if (use_rust) {
            return ObserveRust(instruction, index);
        }
        return ObserveCpp(instruction, index);
    }

    Result ObserveRust(const Instruction& instruction, u32 index) {
        static_assert(sizeof(Instruction) == sizeof(EglAmpereInstruction));
        static_assert(alignof(Instruction) == alignof(EglAmpereInstruction));
        const auto* c_insn = reinterpret_cast<const EglAmpereInstruction*>(&instruction);
        switch (egl_ampere_capture_observe(&rs_state, c_insn, index)) {
        case EGL_AMPERE_CAPTURE_CONTINUE:
            return Result::Continue;
        case EGL_AMPERE_CAPTURE_COMPLETE:
            return Result::Complete;
        case EGL_AMPERE_CAPTURE_INVALID_TARGET:
            return Result::InvalidTarget;
        }
        return Result::InvalidTarget;
    }
#else
    Result Observe(const Instruction& instruction, u32 index) {
        return ObserveCpp(instruction, index);
    }
#endif

    Result ObserveCpp(const Instruction& instruction, u32 index) {
        const u32 opcode = instruction.Bits(0, 12);
        if (index < exit_setup.size()) {
            exit_setup[index] = instruction;
            if (index + 1 == exit_setup.size() && IsGeometryExitSetup()) {
                const u32 target = exit_setup[1].words[1];
                if (target < exit_setup.size() * 16 || target % 16 ||
                    target >= MaximumInstructions * 16)
                    return Result::InvalidTarget;
                last_forward_target = std::max(last_forward_target, target / 16);
                exit_handler_end = target / 16 + 2;
            }
        }
        if (opcode == 0x944 || opcode == 0x947 || opcode == 0x945) {
            // BRA keeps its .DIV/.CONV mode in the offset's two low bits.
            const u32 low = instruction.words[1] & (opcode == 0x947 ? ~3U : ~0U);
            const u64 raw = low | (u64{instruction.words[2] & 0x3ffff} << 32);
            const s64 relative = opcode == 0x945 ? static_cast<s32>(instruction.words[1])
                                                : static_cast<s64>(raw << 14) >> 14;
            const s64 target = s64{index} * 16 + 16 + relative;
            if (target < 0 || target % 16 || target >= MaximumInstructions * 16)
                return Result::InvalidTarget;
            // CALL.REL.NOINC (bit 86) pushes no return address, but its callee still returns
            // with RET.REL.NODEC through the link register set ahead of the call.
            if (opcode == 0x944) {
                last_call_target = std::max(last_call_target, static_cast<u32>(target / 16));
            } else {
                last_forward_target = std::max(last_forward_target, static_cast<u32>(target / 16));
            }
            // The compiler ends every program with an unconditional branch to itself; with a jump
            // table (whose targets live in a constant buffer) that is the only reliable end.
            if (opcode == 0x947 && computed_branch && instruction.Bits(12, 4) == 7 &&
                target == s64{index} * 16)
                return Result::Complete;
        }
        if (opcode == 0x94d && instruction.Bits(12, 4) == 7 && index == exit_handler_end)
            return Result::Complete;
        if (opcode == 0x949 || opcode == 0x958) computed_branch = true;
        if (computed_branch) return Result::Continue;
        if (opcode == 0x94d && instruction.Bits(12, 4) == 7 && index >= last_forward_target) {
            if (index >= last_call_target) return Result::Complete;
            main_exited = true;
        }
        if (opcode == 0x950 && instruction.Bits(12, 4) == 7 && main_exited &&
            index >= last_call_target && index >= last_forward_target)
            return Result::Complete;
        return Result::Continue;
    }

private:
    bool IsGeometryExitSetup() const {
        return std::all_of(exit_setup.begin(), exit_setup.end(),
                           [](const Instruction& setup) {
                               return (setup.words[3] & 0xc00001ffU) == 0;
                           }) &&
               exit_setup[0].words[0] == 0x0000734e && exit_setup[0].words[1] == 0 &&
               exit_setup[0].words[2] == 0 && exit_setup[1].words[0] == 0x00037802 &&
               exit_setup[1].words[2] == 0xf00 && exit_setup[2].words[0] == 0x00007210 &&
               exit_setup[2].words[1] == 3 && exit_setup[2].words[2] == 0x07f1e0ff &&
               exit_setup[3].words[1] == 0xff &&
               ((exit_setup[3].words[0] == 0x01017210 && exit_setup[3].words[2] == 0x007fe4ff) ||
                (exit_setup[3].words[0] == 0xff017224 && exit_setup[3].words[2] == 0x000e0401)) &&
               exit_setup[4].words[0] == 0x00007357 && exit_setup[4].words[1] == 0 &&
               exit_setup[4].words[2] == 0;
    }

    std::array<Instruction, 5> exit_setup{};
    u32 exit_handler_end{};
    u32 last_call_target{};
    u32 last_forward_target{};
    bool main_exited{};
    bool computed_branch{};
#ifdef EGLANTINE_ENABLE_RUST_AMPERE
    bool use_rust{};
    bool verify_rust{};
    EglAmpereCaptureState rs_state{};
#endif
};
} // namespace Shader::Ampere
