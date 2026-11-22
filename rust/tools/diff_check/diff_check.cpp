// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "eglantine_ampere.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/program_override.h"

extern "C" __declspec(dllimport) int __stdcall SetEnvironmentVariableA(const char* name,
                                                                     const char* value);

namespace {

void SetEnvBoth(const char* name, const char* value) {
    _putenv_s(name, value);
    SetEnvironmentVariableA(name, value);
}

using CppInsn = Shader::Ampere::Instruction;
using CppCapture = Shader::Ampere::ProgramCaptureState;
using CppLowered = Shader::Ampere::LoweredProgram;
using CppOptions = Shader::Ampere::LoweringOptions;

static_assert(sizeof(CppInsn) == sizeof(EglAmpereInstruction));
static_assert(alignof(CppInsn) == alignof(EglAmpereInstruction));

constexpr CppInsn kExit{{0x0000794dU, 0U, 0x03800000U, 0U}};
constexpr CppInsn kNop{{0x00007918U, 0U, 0U, 0U}};

struct Stats {
    std::uint64_t cases_run = 0;
    std::uint64_t mismatches = 0;
    std::uint64_t ok_lowered = 0;
    std::uint64_t err_lowered = 0;
};

struct XorShift64 {
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;

    std::uint64_t Next() {
        std::uint64_t x = state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state = x;
        return x;
    }

    std::uint32_t NextU32() {
        return static_cast<std::uint32_t>(Next() >> 16);
    }
};

std::string HexInsn(const CppInsn& insn) {
    std::ostringstream os;
    os << std::hex << "{" << insn.words[0] << ", " << insn.words[1] << ", " << insn.words[2]
       << ", " << insn.words[3] << "}";
    return os.str();
}

void ReportMismatch(Stats& stats, std::string_view label, const std::vector<CppInsn>& code,
                    const std::string& detail) {
    ++stats.mismatches;
    if (stats.mismatches <= 25) {
        std::cerr << "[MISMATCH #" << stats.mismatches << "] " << label << ": " << detail << "\n";
        for (size_t i = 0; i < code.size() && i < 6; ++i) {
            std::cerr << "  insn[" << i << "] = " << HexInsn(code[i]) << "\n";
        }
    }
}

EglAmpereCaptureResult ToCResult(CppCapture::Result res) {
    switch (res) {
    case CppCapture::Result::Continue:
        return EGL_AMPERE_CAPTURE_CONTINUE;
    case CppCapture::Result::Complete:
        return EGL_AMPERE_CAPTURE_COMPLETE;
    case CppCapture::Result::InvalidTarget:
        return EGL_AMPERE_CAPTURE_INVALID_TARGET;
    }
    return EGL_AMPERE_CAPTURE_INVALID_TARGET;
}

void CheckOverrideAndAddressMap(Stats& stats, std::string_view label,
                                const std::vector<CppInsn>& code, const CppLowered& cpp) {
    const auto* c_code = reinterpret_cast<const EglAmpereInstruction*>(code.data());

    const std::uint64_t cpp_key = Shader::Ampere::NativeProgramKey(code);
    const std::uint64_t rs_key = egl_ampere_native_program_key(c_code, code.size());
    if (cpp_key != rs_key) {
        ReportMismatch(stats, label, code, "NativeProgramKey mismatch");
        return;
    }

    const std::vector<u8> cpp_ov = Shader::Ampere::EncodeProgramOverride(code, cpp);
    EglAmpereLoweredProgram rs_in{};
    std::vector<u64> tmp_insns = cpp.instructions;
    std::vector<u32> tmp_offs = cpp.source_instruction_offsets;
    std::string tmp_err = cpp.error;
    rs_in.instructions = tmp_insns.empty() ? nullptr : tmp_insns.data();
    rs_in.instruction_count = tmp_insns.size();
    rs_in.source_instruction_offsets = tmp_offs.empty() ? nullptr : tmp_offs.data();
    rs_in.source_instruction_offset_count = tmp_offs.size();
    rs_in.source_size = cpp.source_size;
    rs_in.required_texture_buffer = cpp.required_texture_buffer;
    rs_in.high_uniform_registers = cpp.high_uniform_registers;
    rs_in.error_offset = cpp.error_offset;
    rs_in.error = tmp_err.data();
    rs_in.error_len = tmp_err.size();

    EglAmpereBytes rs_ov{};
    if (egl_ampere_encode_override(c_code, code.size(), &rs_in, &rs_ov) != 0) {
        ReportMismatch(stats, label, code, "egl_ampere_encode_override returned non-zero");
        egl_ampere_free_bytes(&rs_ov);
        return;
    }

    if (cpp_ov.size() != rs_ov.len ||
        (!cpp_ov.empty() && std::memcmp(cpp_ov.data(), rs_ov.data, cpp_ov.size()) != 0)) {
        ReportMismatch(stats, label, code, "EncodeProgramOverride byte mismatch");
        egl_ampere_free_bytes(&rs_ov);
        return;
    }

    const u32 cpp_src_sz = Shader::Ampere::ProgramOverrideSourceSize(cpp_ov);
    const u32 rs_src_sz = egl_ampere_override_source_size(rs_ov.data, rs_ov.len);
    if (cpp_src_sz != rs_src_sz) {
        ReportMismatch(stats, label, code, "ProgramOverrideSourceSize mismatch");
        egl_ampere_free_bytes(&rs_ov);
        return;
    }

    const CppLowered cpp_dec = Shader::Ampere::DecodeProgramOverride(cpp_ov, code);
    EglAmpereLoweredProgram rs_dec{};
    if (egl_ampere_decode_override(rs_ov.data, rs_ov.len, c_code, code.size(), &rs_dec) != 0) {
        ReportMismatch(stats, label, code, "egl_ampere_decode_override returned non-zero");
        egl_ampere_free_bytes(&rs_ov);
        egl_ampere_free_lowered(&rs_dec);
        return;
    }

    std::string_view rs_dec_err(rs_dec.error ? rs_dec.error : "", rs_dec.error_len);
    if (cpp_dec.error != rs_dec_err || cpp_dec.instructions.size() != rs_dec.instruction_count ||
        (!cpp_dec.instructions.empty() &&
         std::memcmp(cpp_dec.instructions.data(), rs_dec.instructions,
                     cpp_dec.instructions.size() * sizeof(u64)) != 0)) {
        ReportMismatch(stats, label, code, "DecodeProgramOverride mismatch");
    }

    egl_ampere_free_bytes(&rs_ov);
    egl_ampere_free_lowered(&rs_dec);

    if (cpp.error.empty()) {
        std::vector<u64> mapped = cpp.instructions;
        if (mapped.size() < 16) {
            mapped.resize(16, 0ULL);
        }
        Shader::Ampere::AppendNativeAddressMap(mapped, cpp.source_instruction_offsets);
        if (static_cast<bool>(egl_ampere_has_native_address_map(mapped.data(), mapped.size())) !=
            Shader::Ampere::HasNativeAddressMap(mapped)) {
            ReportMismatch(stats, label, code, "HasNativeAddressMap mismatch");
            return;
        }
        for (u32 addr = 0; addr <= static_cast<u32>(code.size() * 16 + 16); addr += 4) {
            const auto cpp_target = Shader::Ampere::NativeProgramAddress(mapped, addr);
            u32 rs_target = 0;
            const bool rs_found =
                egl_ampere_native_program_address(mapped.data(), mapped.size(), addr, &rs_target) !=
                0;
            if (cpp_target.has_value() != rs_found ||
                (rs_found && *cpp_target != rs_target)) {
                ReportMismatch(stats, label, code, "NativeProgramAddress mismatch");
                return;
            }
        }
    }
}

void CompareCase(Stats& stats, std::string_view label, const std::vector<CppInsn>& code,
                 CppOptions options = {}, int8_t lower_extended_override = -1,
                 bool check_codec = false) {
    ++stats.cases_run;

    const CppLowered cpp = Shader::Ampere::LowerToMaxwell(code, options);
    if (cpp) {
        ++stats.ok_lowered;
    } else {
        ++stats.err_lowered;
    }

    const EglAmpereLoweringOptions c_opts{
        .geometry = static_cast<uint8_t>(options.geometry ? 1 : 0),
        .tensor = static_cast<uint8_t>(options.tensor ? 1 : 0),
        .compact_tensor = static_cast<uint8_t>(options.compact_tensor ? 1 : 0),
        .lower_extended_override = lower_extended_override,
        ._pad = {0, 0, 0, 0},
        .native_code_address = options.native_code_address,
    };

    const auto* c_code =
        code.empty() ? nullptr : reinterpret_cast<const EglAmpereInstruction*>(code.data());
    EglAmpereLoweredProgram rs{};
    const int32_t rc = egl_ampere_lower_to_maxwell(c_code, code.size(), &c_opts, &rs);
    if (rc != 0) {
        ReportMismatch(stats, label, code, "egl_ampere_lower_to_maxwell returned non-zero");
        egl_ampere_free_lowered(&rs);
        return;
    }

    const std::string_view rs_err(rs.error ? rs.error : "", rs.error_len);
    if (cpp.error != rs_err) {
        std::ostringstream os;
        os << "error string mismatch: cpp='" << cpp.error << "' rs='" << rs_err << "'";
        ReportMismatch(stats, label, code, os.str());
        egl_ampere_free_lowered(&rs);
        return;
    }

    if (cpp.error_offset != rs.error_offset) {
        std::ostringstream os;
        os << "error_offset mismatch: cpp=" << cpp.error_offset << " rs=" << rs.error_offset;
        ReportMismatch(stats, label, code, os.str());
        egl_ampere_free_lowered(&rs);
        return;
    }

    if (cpp.source_size != rs.source_size) {
        std::ostringstream os;
        os << "source_size mismatch: cpp=" << cpp.source_size << " rs=" << rs.source_size;
        ReportMismatch(stats, label, code, os.str());
        egl_ampere_free_lowered(&rs);
        return;
    }

    if (cpp.required_texture_buffer != rs.required_texture_buffer) {
        std::ostringstream os;
        os << "required_texture_buffer mismatch: cpp=" << cpp.required_texture_buffer
           << " rs=" << rs.required_texture_buffer;
        ReportMismatch(stats, label, code, os.str());
        egl_ampere_free_lowered(&rs);
        return;
    }

    if (cpp.high_uniform_registers != rs.high_uniform_registers) {
        std::ostringstream os;
        os << "high_uniform_registers mismatch: cpp=" << cpp.high_uniform_registers
           << " rs=" << rs.high_uniform_registers;
        ReportMismatch(stats, label, code, os.str());
        egl_ampere_free_lowered(&rs);
        return;
    }

    if (cpp.source_instruction_offsets.size() != rs.source_instruction_offset_count) {
        std::ostringstream os;
        os << "source_instruction_offsets len mismatch: cpp="
           << cpp.source_instruction_offsets.size()
           << " rs=" << rs.source_instruction_offset_count;
        ReportMismatch(stats, label, code, os.str());
        egl_ampere_free_lowered(&rs);
        return;
    }
    for (size_t i = 0; i < cpp.source_instruction_offsets.size(); ++i) {
        if (cpp.source_instruction_offsets[i] != rs.source_instruction_offsets[i]) {
            std::ostringstream os;
            os << "source_instruction_offsets[" << i
               << "] mismatch: cpp=" << cpp.source_instruction_offsets[i]
               << " rs=" << rs.source_instruction_offsets[i];
            ReportMismatch(stats, label, code, os.str());
            egl_ampere_free_lowered(&rs);
            return;
        }
    }

    if (cpp.instructions.size() != rs.instruction_count) {
        std::ostringstream os;
        os << "instructions len mismatch: cpp=" << cpp.instructions.size()
           << " rs=" << rs.instruction_count;
        ReportMismatch(stats, label, code, os.str());
        egl_ampere_free_lowered(&rs);
        return;
    }
    for (size_t i = 0; i < cpp.instructions.size(); ++i) {
        if (cpp.instructions[i] != rs.instructions[i]) {
            std::ostringstream os;
            os << std::hex << "instructions[" << std::dec << i << std::hex << "] mismatch: cpp=0x"
               << cpp.instructions[i] << " rs=0x" << rs.instructions[i];
            ReportMismatch(stats, label, code, os.str());
            egl_ampere_free_lowered(&rs);
            return;
        }
    }

    if (check_codec) {
        CheckOverrideAndAddressMap(stats, label, code, cpp);
    }

    egl_ampere_free_lowered(&rs);
}

void RunCaptureSuite(Stats& stats) {
    const std::vector<std::vector<CppInsn>> sequences{
        {kExit},
        {kNop, kExit},
        {CppInsn{{0x00007947U, 16U, 0U, 0U}}, kExit, kExit},
        {CppInsn{{0x00007944U, 16U, 0U, 0U}}, kExit,
         CppInsn{{0x00007950U, 0xffffffc0U, 0x03c3ffffU, 0U}}},
        {CppInsn{{0x00007949U, 0U, 0U, 0U}}, kExit,
         CppInsn{{0x00007947U, static_cast<u32>(-16), 0x0003ffffU, 0U}}},
        {CppInsn{{0x00007947U, static_cast<u32>(-32), 0x0003ffffU, 0U}}},
        {CppInsn{{0x00007945U, 4U, 0U, 0U}}},
        {CppInsn{{0x00007945U, CppCapture::MaximumInstructions * 16U, 0U, 0U}}},
    };

    for (size_t s = 0; s < sequences.size(); ++s) {
        ++stats.cases_run;
        CppCapture cpp_cap;
        EglAmpereCaptureState rs_cap{};
        egl_ampere_capture_init(&rs_cap);
        for (u32 idx = 0; idx < sequences[s].size(); ++idx) {
            const auto cpp_res = cpp_cap.Observe(sequences[s][idx], idx);
            const auto* c_insn = reinterpret_cast<const EglAmpereInstruction*>(&sequences[s][idx]);
            const auto rs_res = egl_ampere_capture_observe(&rs_cap, c_insn, idx);
            if (ToCResult(cpp_res) != rs_res) {
                ReportMismatch(stats, "capture_sequence_" + std::to_string(s), sequences[s],
                               "CaptureResult mismatch at index " + std::to_string(idx));
                break;
            }
            if (cpp_res != CppCapture::Result::Continue) {
                break;
            }
        }
    }
}

void RunHandcraftedSuite(Stats& stats) {
    // Empty and no-exit error paths
    CompareCase(stats, "empty", {}, {});
    CompareCase(stats, "no_exit", {kNop}, {});

    // Core control flow + call/ret
    CompareCase(stats, "exit_only", {kExit}, {}, -1, true);
    CompareCase(
        stats, "call_ret",
        {
            CppInsn{{0x00007802U, 0x20U, 0x00000f00U, 0U}},
            CppInsn{{0x00007944U, 0x10U, 0x03c00000U, 0U}},
            kExit,
            CppInsn{{0x00007950U, 0xffffffc0U, 0x03c3ffffU, 0U}},
        },
        {}, -1, true);

    // I2F / F2I / LDC / ULDC / SURED / VOTE
    CompareCase(stats, "i2f_1",
                {CppInsn{{0x00017306U, 0x00000001U, 0x00101400U, 0x020ea200U}}, kExit}, {}, -1,
                true);
    CompareCase(stats, "i2f_2",
                {CppInsn{{0x00037306U, 0x00000003U, 0x00101400U, 0x000e2400U}}, kExit});
    CompareCase(stats, "i2f_3", {CppInsn{{0x00017306U, 0x00000001U, 0x00101000U, 0U}}, kExit});
    CompareCase(stats, "i2f_4",
                {CppInsn{{0x00027d06U, 0x00000004U, 0x08101000U, 0x000e6200U}}, kExit});

    for (bool is_signed : {false, true}) {
        CompareCase(stats, is_signed ? "f2i_s" : "f2i_u",
                    {CppInsn{{0x00067305U, 0x00000009U,
                              is_signed ? 0x0021b100U : 0x0020b000U, 0U}},
                     kExit});
    }
    CompareCase(stats, "f2i_unsupported",
                {CppInsn{{0x00067305U, 0x00000009U, 0x0020b001U, 0U}}, kExit});
    CompareCase(stats, "f2i_alt",
                {CppInsn{{0x00097305U, 0x00000005U, 0x0021e100U, 0U}}, kExit});

    for (u32 mode : {0U, 1U, 2U}) {
        for (u32 width = 0; width <= 6; ++width) {
            CompareCase(stats, "ldc",
                        {CppInsn{{0x02147b82U, 0U, (width << 9) | (mode << 14), 0U}}, kExit});
        }
    }
    CompareCase(stats, "ldc_odd_pair",
                {CppInsn{{0x02157b82U, 0U, 5U << 9, 0U}}, kExit});
    CompareCase(stats, "uldc_u16",
                {CppInsn{{0x00047ab9U, 0x01800800U, 0x00000400U, 0x000fe200U}}, kExit}, {}, -1,
                true);

    // Geometry
    CompareCase(stats, "geometry",
                {
                    CppInsn{{0x0004734eU, 0U, 0U, 0U}},
                    CppInsn{{0x00017923U, 0U, 0U, 0U}},
                    CppInsn{{0x00027324U, 0xffU, 0x4000U, 0U}},
                    CppInsn{{0x02037924U, 0U, 0x8000U, 0U}},
                    kExit,
                },
                CppOptions{.geometry = true, .native_code_address = 0x00000012fffffff0ULL}, -1,
                true);

    // SURED across dimensions and guards + error
    for (u32 dim = 0; dim < 4; ++dim) {
        CompareCase(stats, "sured_dim",
                    {CppInsn{{0x0000759fU, (0xa0002203U & 0x1fffffffU) | (dim << 29),
                              0x0010e000U, 0x0011e600U}},
                     kExit});
    }
    CompareCase(stats, "sured_r253_err",
                {CppInsn{{0xfd00759fU, 0xa0002203U, 0x0010e000U, 0x0011e600U}}, kExit});

    // Uniform LEA, USEL, BREV, LEA, STG, HMMA, register pressure, UR28+
    CompareCase(stats, "ulea_guard_up2",
                {CppInsn{{0x291U | (10U << 12) | (12U << 16) | (11U << 24), 19U,
                          63U | (3U << 11) | (7U << 17) | (1U << 27), 0U}},
                 kExit},
                {}, -1, true);
    CompareCase(stats, "ulea_ur30_retry",
                {CppInsn{{0x291U | (7U << 12) | (30U << 16) | (11U << 24), 19U,
                          63U | (3U << 11) | (7U << 17) | (1U << 27), 0U}},
                 kExit},
                {}, -1, true);
    CompareCase(stats, "usel_reg",
                {CppInsn{{0x287U | (7U << 12) | (4U << 16) | (6U << 24), 8U,
                          0x08000000U | (1U << 23), 0U}},
                 kExit});
    CompareCase(stats, "lea_stg",
                {CppInsn{{0x08087211U, 0x00000007U, 0x078ff8ffU, 0x000fc800U}},
                 CppInsn{{0x986U | (7U << 12) | (254U << 24), 0U, 0x0810a900U, 0U}}, kExit},
                {}, -1, true);
    CompareCase(
        stats, "high_reg_pressure",
        {CppInsn{{0x221U | (7U << 12) | (240U << 16) | (242U << 24), 244U, 0U, 0U}}, kExit}, {},
        -1, true);

    const std::vector<CppInsn> hmma{CppInsn{{0x0004723cU, 0x00000008U, 0U, 0U}}, kExit};
    CompareCase(stats, "hmma_disabled", hmma, CppOptions{.tensor = false});
    CompareCase(stats, "hmma_inline", hmma, CppOptions{.tensor = true, .compact_tensor = false}, -1,
                true);
    CompareCase(stats, "hmma_compact", hmma, CppOptions{.tensor = true, .compact_tensor = true}, -1,
                true);

    // Toggle EDEN_NX2_LOWER_EXTENDED=0 vs 1 and confirm C++ and Rust follow dynamically
    SetEnvBoth("EDEN_NX2_LOWER_EXTENDED", "0");
    CompareCase(stats, "ext_off_uldc",
                {CppInsn{{0x00047ab9U, 0x01800800U, 0x00000400U, 0x000fe200U}}, kExit}, {}, 0);
    CompareCase(stats, "ext_off_i2f",
                {CppInsn{{0x00027d06U, 0x00000004U, 0x08101000U, 0x000e6200U}}, kExit}, {}, -1);
    CompareCase(stats, "ext_off_ulea_up3",
                {CppInsn{{0x291U | (7U << 12) | (12U << 16) | (11U << 24), 19U,
                          63U | (3U << 11) | (3U << 17) | (1U << 27), 0U}},
                 kExit},
                {}, 0);
    SetEnvBoth("EDEN_NX2_LOWER_EXTENDED", "1");
    CompareCase(stats, "ext_on_ulea_up3",
                {CppInsn{{0x291U | (7U << 12) | (12U << 16) | (11U << 24), 19U,
                          63U | (3U << 11) | (3U << 17) | (1U << 27), 0U}},
                 kExit},
                {}, 1);
}

void RunOpcodeSweep(Stats& stats) {
    // All 230 unique switch opcodes + 11 uniform pre-rewrite opcodes = 241 opcodes.
    static constexpr std::array<u32, 241> kOpcodes{
        0x202, 0x207, 0x208, 0x209, 0x20a, 0x20b, 0x20c, 0x210, 0x211, 0x212, 0x213, 0x217, 0x219,
        0x220, 0x221, 0x223, 0x224, 0x225, 0x226, 0x227, 0x230, 0x231, 0x232, 0x233, 0x234, 0x23a,
        0x23c, 0x23e, 0x240, 0x243, 0x245, 0x287, 0x28c, 0x290, 0x291, 0x292, 0x299, 0x2a4, 0x2a5,
        0x2bd, 0x2be, 0x2bf, 0x300, 0x301, 0x304, 0x305, 0x306, 0x307, 0x308, 0x309, 0x311, 0x312,
        0x321, 0x322, 0x324, 0x326, 0x348, 0x34e, 0x357, 0x361, 0x367, 0x36a, 0x370, 0x381, 0x386,
        0x387, 0x388, 0x38c, 0x3a1, 0x3a8, 0x3c2, 0x3c4, 0x419, 0x421, 0x423, 0x424, 0x430, 0x431,
        0x433, 0x434, 0x499, 0x4a4, 0x526, 0x589, 0x593, 0x59f, 0x621, 0x623, 0x624, 0x625, 0x630,
        0x631, 0x633, 0x634, 0x802, 0x803, 0x804, 0x805, 0x806, 0x807, 0x808, 0x809, 0x80a, 0x80b,
        0x80c, 0x80d, 0x810, 0x811, 0x812, 0x816, 0x817, 0x819, 0x81a, 0x81c, 0x820, 0x821, 0x822,
        0x823, 0x824, 0x825, 0x827, 0x831, 0x832, 0x83b, 0x840, 0x845, 0x882, 0x883, 0x886, 0x887,
        0x88c, 0x890, 0x891, 0x892, 0x896, 0x899, 0x89c, 0x8a4, 0x8a5, 0x906, 0x918, 0x919, 0x91a,
        0x923, 0x924, 0x927, 0x941, 0x942, 0x944, 0x945, 0x947, 0x948, 0x949, 0x94d, 0x950, 0x957,
        0x958, 0x95b, 0x95c, 0x981, 0x983, 0x984, 0x986, 0x988, 0x989, 0x98e, 0x992, 0x9a8, 0x9ab,
        0x9af, 0x9c3, 0xa02, 0xa07, 0xa08, 0xa09, 0xa0a, 0xa0b, 0xa0c, 0xa10, 0xa11, 0xa12, 0xa13,
        0xa17, 0xa19, 0xa20, 0xa21, 0xa23, 0xa24, 0xa27, 0xa31, 0xa32, 0xa3e, 0xa45, 0xab9, 0xabb,
        0xb05, 0xb07, 0xb08, 0xb1d, 0xb60, 0xb63, 0xb66, 0xb69, 0xb6c, 0xb6f, 0xb82, 0xb97, 0xb9b,
        0xc02, 0xc07, 0xc09, 0xc0a, 0xc0b, 0xc0c, 0xc10, 0xc11, 0xc12, 0xc13, 0xc17, 0xc19, 0xc20,
        0xc23, 0xc24, 0xc45, 0xc82, 0xd00, 0xd05, 0xd06, 0xd08, 0xd09, 0xe21, 0xe23, 0xe24, 0xf60,
        0xf63, 0xf66, 0xf69, 0xf6c, 0xf89, 0xf8c, 0xfae,
    };

    XorShift64 rng{};
    for (const u32 opcode : kOpcodes) {
        for (int variant = 0; variant < 24; ++variant) {
            CppInsn insn{};
            if (variant == 0) {
                // Clean low registers, predicate 7, zeroed high words
                insn.words[0] = opcode | (7U << 12) | (2U << 16) | (4U << 24);
                insn.words[1] = 6U;
                insn.words[2] = 8U;
                insn.words[3] = 0U;
            } else if (variant == 1) {
                // Clean zero-src / UR-friendly form
                insn.words[0] = opcode | (7U << 12) | (4U << 16) | (6U << 24);
                insn.words[1] = 0x00000008U;
                insn.words[2] = 0x0010000aU;
                insn.words[3] = 0x000fe200U;
            } else if (variant == 2) {
                // High registers + guarded uniform predicate + UR28+
                insn.words[0] = opcode | (2U << 12) | (30U << 16) | (242U << 24);
                insn.words[1] = 244U | (0x080U << 20);
                insn.words[2] = 30U | (3U << 17) | (1U << 27);
                insn.words[3] = 0U;
            } else if (variant == 3) {
                //RZ / RZ sources & dest
                insn.words[0] = opcode | (7U << 12) | (255U << 16) | (255U << 24);
                insn.words[1] = 255U;
                insn.words[2] = 255U;
                insn.words[3] = 0U;
            } else {
                // Structured random fields biased toward valid register / predicate / cbuf ranges
                const u32 pred = (variant % 5 == 0) ? 7U : (rng.NextU32() & 0xfU);
                const u32 dst = (variant % 4 == 0) ? (238U + (rng.NextU32() % 18U))
                                                   : (rng.NextU32() & 0x3fU);
                const u32 src_a = (rng.NextU32() % 3 == 0) ? 255U : (rng.NextU32() & 0x3fU);
                insn.words[0] = opcode | (pred << 12) | (dst << 16) | (src_a << 24);
                insn.words[1] = rng.NextU32();
                insn.words[2] = rng.NextU32();
                insn.words[3] = rng.NextU32();
                if (variant % 2 == 0) {
                    insn.words[1] &= 0x00ffffffU;
                    insn.words[2] &= 0x03ffffffU;
                    insn.words[3] &= 0x000fffffU;
                }
            }

            CppOptions opts{};
            opts.geometry = (variant % 6 == 0);
            opts.tensor = (variant % 3 == 0);
            opts.compact_tensor = (variant % 6 == 0);
            opts.native_code_address = (variant % 4 == 0) ? 0x00000012fffffff0ULL : 0ULL;

            const std::vector<CppInsn> prog{insn, kExit};
            CompareCase(stats, "sweep_0x" + [&] {
                std::ostringstream os;
                os << std::hex << opcode << "_v" << std::dec << variant;
                return os.str();
            }(), prog, opts);
        }
    }

    // Multi-instruction programs with branches, pressure, uniform mirrors
    for (int p = 0; p < 350; ++p) {
        const size_t len = 2 + (rng.NextU32() % 10);
        std::vector<CppInsn> prog;
        prog.reserve(len + 1);
        for (size_t i = 0; i < len; ++i) {
            const u32 op = kOpcodes[rng.NextU32() % kOpcodes.size()];
            CppInsn insn{};
            if (op == 0x947 || op == 0x944) {
                const s32 rel = static_cast<s32>((rng.NextU32() % 5)) * 16;
                insn.words[0] = op | (7U << 12);
                insn.words[1] = static_cast<u32>(rel);
            } else {
                const u32 pred = (p % 2 == 0) ? 7U : (rng.NextU32() & 7U);
                const u32 dst = rng.NextU32() & 0x3fU;
                const u32 src_a = rng.NextU32() & 0x3fU;
                insn.words[0] = op | (pred << 12) | (dst << 16) | (src_a << 24);
                insn.words[1] = rng.NextU32() & 0x00ffffffU;
                insn.words[2] = rng.NextU32() & 0x03ffffffU;
                insn.words[3] = rng.NextU32() & 0x000fffffU;
            }
            prog.push_back(insn);
        }
        prog.push_back(kExit);

        CppOptions opts{};
        opts.geometry = (p % 4 == 0);
        opts.tensor = (p % 3 == 0);
        opts.compact_tensor = (p % 6 == 0);
        opts.native_code_address = 0x0000001000000000ULL;
        CompareCase(stats, "multi_" + std::to_string(p), prog, opts);
    }
}

void RunDecodeMalformedSuite(Stats& stats) {
    // Compare malformed / mismatching override buffers
    const std::vector<CppInsn> source{
        CppInsn{{0x00017306U, 0x00000001U, 0x00101400U, 0x020ea200U}},
        kExit,
    };
    const CppLowered cpp = Shader::Ampere::LowerToMaxwell(source, {});
    std::vector<u8> bytes = Shader::Ampere::EncodeProgramOverride(source, cpp);
    const auto* c_src = reinterpret_cast<const EglAmpereInstruction*>(source.data());

    std::vector<std::vector<u8>> mutated{
        {},
        {1, 2, 3, 4},
        bytes,
    };
    if (mutated.back().size() > 8) {
        mutated.back().pop_back();
    }
    if (bytes.size() > 28) {
        auto flip = bytes;
        flip[24] ^= 0xff;
        mutated.push_back(flip);
        auto bad_magic = bytes;
        bad_magic[0] ^= 1;
        mutated.push_back(bad_magic);
    }

    for (size_t i = 0; i < mutated.size(); ++i) {
        ++stats.cases_run;
        const auto& buf = mutated[i];
        const CppLowered c_dec = Shader::Ampere::DecodeProgramOverride(buf, source);
        EglAmpereLoweredProgram r_dec{};
        if (egl_ampere_decode_override(buf.empty() ? nullptr : buf.data(), buf.size(), c_src,
                                       source.size(), &r_dec) != 0) {
            ReportMismatch(stats, "malformed_override_" + std::to_string(i), source,
                           "decode returned non-zero");
        } else {
            const std::string_view r_err(r_dec.error ? r_dec.error : "", r_dec.error_len);
            if (c_dec.error != r_err) {
                ReportMismatch(stats, "malformed_override_" + std::to_string(i), source,
                               "error string mismatch: cpp='" + c_dec.error + "' rs='" +
                                   std::string(r_err) + "'");
            }
        }
        egl_ampere_free_lowered(&r_dec);
    }
}

} // namespace

int main() {
    Stats stats{};
    RunCaptureSuite(stats);
    RunHandcraftedSuite(stats);
    RunDecodeMalformedSuite(stats);
    RunOpcodeSweep(stats);

    std::cout << "diff_check finished: cases=" << stats.cases_run
              << " ok_lowered=" << stats.ok_lowered << " err_lowered=" << stats.err_lowered
              << " mismatches=" << stats.mismatches << "\n";
    return stats.mismatches == 0 ? 0 : 1;
}
