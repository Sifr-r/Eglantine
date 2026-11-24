// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstring>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace Shader::Ampere {

inline u64 NativeProgramKeyCpp(std::span<const Instruction> source) {
    u64 hash = 14695981039346656037ULL;
    for (const auto& instruction : source) {
        for (u32 word : instruction.words) {
            for (u32 byte = 0; byte < 4; ++byte) hash = (hash ^ ((word >> (byte * 8)) & 255)) * 1099511628211ULL;
        }
    }
    return hash;
}

// Private iteration format: version, lengths, texture bank, exact original
// instructions, then the newly generated canonical instructions. Source bytes
// are compared directly; the filename hash is only an index.
inline std::vector<u8> EncodeProgramOverrideCpp(std::span<const Instruction> source,
                                                const LoweredProgram& program) {
    if (!program || program.source_size != source.size_bytes() || program.source_instruction_offsets.size() != source.size()) return {};
    const std::array<u32, 6> header{0x4e454445, 0x3230564f,
        static_cast<u32>(source.size_bytes()), static_cast<u32>(program.instructions.size()),
        program.required_texture_buffer, static_cast<u32>(program.source_instruction_offsets.size())};
    std::vector<u8> bytes(sizeof(header) + source.size_bytes() + program.instructions.size() * 8 + program.source_instruction_offsets.size() * 4);
    std::memcpy(bytes.data(), header.data(), sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), source.data(), source.size_bytes());
    std::memcpy(bytes.data() + sizeof(header) + source.size_bytes(), program.instructions.data(), program.instructions.size() * 8);
    std::memcpy(bytes.data() + sizeof(header) + source.size_bytes() + program.instructions.size() * 8,
                program.source_instruction_offsets.data(), program.source_instruction_offsets.size() * 4);
    return bytes;
}

inline u32 ProgramOverrideSourceSizeCpp(std::span<const u8> bytes) {
    std::array<u32, 6> header{};
    if (bytes.size() < sizeof(header)) return 0;
    std::memcpy(header.data(), bytes.data(), sizeof(header));
    const bool mapped = header[1] == 0x3230564f;
    if (header[0] != 0x4e454445 || (!mapped && header[1] != 0x3130564f) ||
        (mapped ? header[5] != header[2] / 16 : header[5] != 0) ||
        header[2] == 0 || header[2] % sizeof(Instruction) || header[2] > 1048576 ||
        header[3] == 0 || header[3] > 1048576 || (header[4] != ~0U && header[4] >= 18)) return 0;
    if (bytes.size() != sizeof(header) + size_t{header[2]} + size_t{header[3]} * 8 + size_t{header[5]} * 4) return 0;
    return header[2];
}

inline LoweredProgram DecodeProgramOverrideCpp(std::span<const u8> bytes,
                                                std::span<const Instruction> source) {
    LoweredProgram result;
    const auto fail = [&result](std::string_view message) {
        result.instructions.clear();
        result.source_instruction_offsets.clear();
        result.error = message;
        return result;
    };
    std::array<u32, 6> header{};
    if (!ProgramOverrideSourceSizeCpp(bytes)) return fail("invalid native override header or length");
    std::memcpy(header.data(), bytes.data(), sizeof(header));
    if (header[2] != source.size_bytes()) return fail("native override source length mismatch");
    if (std::memcmp(bytes.data() + sizeof(header), source.data(), source.size_bytes()))
        return fail("native override source mismatch");
    result.instructions.resize(header[3]);
    std::memcpy(result.instructions.data(), bytes.data() + sizeof(header) + source.size_bytes(), header[3] * 8);
    if (header[5]) {
        result.source_instruction_offsets.resize(header[5]);
        std::memcpy(result.source_instruction_offsets.data(),
                    bytes.data() + sizeof(header) + source.size_bytes() + size_t{header[3]} * 8,
                    size_t{header[5]} * 4);
        u32 previous = 0;
        for (u32 offset : result.source_instruction_offsets) {
            if (offset >= result.instructions.size() || offset < previous)
                return fail("native override address map mismatch");
            previous = offset;
        }
    }
    result.source_size = header[2];
    result.required_texture_buffer = header[4];
    return result;
}

#ifdef EGLANTINE_ENABLE_RUST_AMPERE

namespace detail {

struct ScopedAmpereBytes {
    EglAmpereBytes raw{};
    ScopedAmpereBytes() = default;
    ~ScopedAmpereBytes() {
        egl_ampere_free_bytes(&raw);
    }
    ScopedAmpereBytes(const ScopedAmpereBytes&) = delete;
    ScopedAmpereBytes& operator=(const ScopedAmpereBytes&) = delete;
};

inline bool TryEncodeProgramOverrideRust(std::span<const Instruction> source,
                                         const LoweredProgram& program, std::vector<u8>& out,
                                         int32_t& out_rc) {
    EglAmpereLoweredProgram raw_prog{};
    raw_prog.instructions =
        program.instructions.empty() ? nullptr : const_cast<u64*>(program.instructions.data());
    raw_prog.instruction_count = program.instructions.size();
    raw_prog.source_instruction_offsets =
        program.source_instruction_offsets.empty()
            ? nullptr
            : const_cast<u32*>(program.source_instruction_offsets.data());
    raw_prog.source_instruction_offset_count = program.source_instruction_offsets.size();
    raw_prog.source_size = program.source_size;
    raw_prog.required_texture_buffer = program.required_texture_buffer;
    raw_prog.high_uniform_registers = program.high_uniform_registers;
    raw_prog.error_offset = program.error_offset;
    raw_prog.error = program.error.empty() ? nullptr : const_cast<char*>(program.error.data());
    raw_prog.error_len = program.error.size();

    ScopedAmpereBytes guard;
    const auto* c_source =
        source.empty() ? nullptr : reinterpret_cast<const EglAmpereInstruction*>(source.data());
    out_rc = egl_ampere_encode_override(c_source, source.size(), &raw_prog, &guard.raw);
    if (out_rc != 0 || (guard.raw.len > 0 && guard.raw.data == nullptr)) {
        out.clear();
        return false;
    }
    if (guard.raw.len > 0) {
        out.assign(guard.raw.data, guard.raw.data + guard.raw.len);
    } else {
        out.clear();
    }
    return true;
}

inline bool TryDecodeProgramOverrideRust(std::span<const u8> bytes,
                                         std::span<const Instruction> source, LoweredProgram& out,
                                         int32_t& out_rc) {
    ScopedAmpereLoweredProgram guard;
    const auto* c_source =
        source.empty() ? nullptr : reinterpret_cast<const EglAmpereInstruction*>(source.data());
    out_rc = egl_ampere_decode_override(bytes.empty() ? nullptr : bytes.data(), bytes.size(),
                                        c_source, source.size(), &guard.raw);
    if (out_rc != 0 || !CopyFromRustLowered(guard.raw, out)) {
        out.instructions.clear();
        out.source_instruction_offsets.clear();
        out.error = "rust ampere FFI override decode error";
        return false;
    }
    return true;
}

} // namespace detail

inline u64 NativeProgramKeyRust(std::span<const Instruction> source) {
    const auto* c_source =
        source.empty() ? nullptr : reinterpret_cast<const EglAmpereInstruction*>(source.data());
    return egl_ampere_native_program_key(c_source, source.size());
}

inline u64 NativeProgramKey(std::span<const Instruction> source) {
    if (detail::IsEnvFlagEnabled("EDEN_NX2_RUST_AMPERE_VERIFY")) {
        const u64 cpp_key = NativeProgramKeyCpp(source);
        const u64 rust_key = NativeProgramKeyRust(source);
        if (cpp_key != rust_key) {
            detail::AbortVerifyMismatch(
                source, "NativeProgramKey",
                fmt::format("cpp={:016x} rust={:016x}", cpp_key, rust_key));
        }
        return cpp_key;
    }
    if (detail::IsEnvFlagEnabled("EDEN_NX2_RUST_AMPERE")) {
        return NativeProgramKeyRust(source);
    }
    return NativeProgramKeyCpp(source);
}

inline std::vector<u8> EncodeProgramOverrideRust(std::span<const Instruction> source,
                                                 const LoweredProgram& program) {
    std::vector<u8> out;
    int32_t rc = 0;
    if (!detail::TryEncodeProgramOverrideRust(source, program, out, rc)) {
        std::fprintf(stderr, "[EDEN_NX2_RUST_AMPERE] egl_ampere_encode_override failed (rc=%d)\n",
                     static_cast<int>(rc));
        std::fflush(stderr);
    }
    return out;
}

inline std::vector<u8> EncodeProgramOverride(std::span<const Instruction> source,
                                             const LoweredProgram& program) {
    if (detail::IsEnvFlagEnabled("EDEN_NX2_RUST_AMPERE_VERIFY")) {
        std::vector<u8> cpp_bytes = EncodeProgramOverrideCpp(source, program);
        std::vector<u8> rust_bytes;
        int32_t rc = 0;
        if (!detail::TryEncodeProgramOverrideRust(source, program, rust_bytes, rc)) {
            detail::AbortVerifyMismatch(
                source, "EncodeProgramOverride",
                fmt::format("egl_ampere_encode_override FFI failure (rc={})", rc));
        }
        if (cpp_bytes != rust_bytes) {
            detail::AbortVerifyMismatch(
                source, "EncodeProgramOverride",
                fmt::format("byte mismatch: cpp_len={} rust_len={}", cpp_bytes.size(),
                            rust_bytes.size()));
        }
        return cpp_bytes;
    }
    if (detail::IsEnvFlagEnabled("EDEN_NX2_RUST_AMPERE")) {
        return EncodeProgramOverrideRust(source, program);
    }
    return EncodeProgramOverrideCpp(source, program);
}

inline u32 ProgramOverrideSourceSizeRust(std::span<const u8> bytes) {
    return egl_ampere_override_source_size(bytes.empty() ? nullptr : bytes.data(), bytes.size());
}

inline u32 ProgramOverrideSourceSize(std::span<const u8> bytes) {
    if (detail::IsEnvFlagEnabled("EDEN_NX2_RUST_AMPERE_VERIFY")) {
        const u32 cpp_size = ProgramOverrideSourceSizeCpp(bytes);
        const u32 rust_size = ProgramOverrideSourceSizeRust(bytes);
        if (cpp_size != rust_size) {
            std::fprintf(stderr,
                         "[EDEN_NX2_RUST_AMPERE_VERIFY] ProgramOverrideSourceSize mismatch "
                         "(len=%zu): cpp=%u rust=%u\n",
                         bytes.size(), static_cast<unsigned>(cpp_size),
                         static_cast<unsigned>(rust_size));
            std::fflush(stderr);
            std::abort();
        }
        return cpp_size;
    }
    if (detail::IsEnvFlagEnabled("EDEN_NX2_RUST_AMPERE")) {
        return ProgramOverrideSourceSizeRust(bytes);
    }
    return ProgramOverrideSourceSizeCpp(bytes);
}

inline LoweredProgram DecodeProgramOverrideRust(std::span<const u8> bytes,
                                                std::span<const Instruction> source) {
    LoweredProgram out;
    int32_t rc = 0;
    if (!detail::TryDecodeProgramOverrideRust(bytes, source, out, rc)) {
        std::fprintf(stderr, "[EDEN_NX2_RUST_AMPERE] egl_ampere_decode_override failed (rc=%d)\n",
                     static_cast<int>(rc));
        std::fflush(stderr);
    }
    return out;
}

inline LoweredProgram DecodeProgramOverride(std::span<const u8> bytes,
                                            std::span<const Instruction> source) {
    if (detail::IsEnvFlagEnabled("EDEN_NX2_RUST_AMPERE_VERIFY")) {
        LoweredProgram cpp_prog = DecodeProgramOverrideCpp(bytes, source);
        LoweredProgram rust_prog;
        int32_t rc = 0;
        if (!detail::TryDecodeProgramOverrideRust(bytes, source, rust_prog, rc)) {
            detail::AbortVerifyMismatch(
                source, "DecodeProgramOverride",
                fmt::format("egl_ampere_decode_override FFI failure (rc={})", rc));
        }
        detail::VerifyLoweredProgramsMatch(source, cpp_prog, rust_prog, "DecodeProgramOverride");
        return cpp_prog;
    }
    if (detail::IsEnvFlagEnabled("EDEN_NX2_RUST_AMPERE")) {
        return DecodeProgramOverrideRust(bytes, source);
    }
    return DecodeProgramOverrideCpp(bytes, source);
}

#else

inline u64 NativeProgramKey(std::span<const Instruction> source) {
    return NativeProgramKeyCpp(source);
}

inline std::vector<u8> EncodeProgramOverride(std::span<const Instruction> source,
                                             const LoweredProgram& program) {
    return EncodeProgramOverrideCpp(source, program);
}

inline u32 ProgramOverrideSourceSize(std::span<const u8> bytes) {
    return ProgramOverrideSourceSizeCpp(bytes);
}

inline LoweredProgram DecodeProgramOverride(std::span<const u8> bytes,
                                            std::span<const Instruction> source) {
    return DecodeProgramOverrideCpp(bytes, source);
}

#endif

} // namespace Shader::Ampere
