// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>

#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/program_override.h"
#include "video_core/shader_environment.h"
#include "video_core/shader_warm/warm_records.h"

namespace VideoCommon::ShaderWarm {
namespace {

/// The shader header a compute environment holds in front of its code (all zero).
constexpr size_t ComputeHeaderWords = 10;
constexpr size_t EnvHeaderSize = 6 * 8 + 7 * 4;
constexpr u32 ComputeStage = 5;

template <typename T>
void Put(std::vector<u8>& out, T value) {
    const size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

void PutBytes(std::vector<u8>& out, std::span<const u8> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

template <typename T>
T Get(std::span<const u8> data, size_t offset) {
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

/// The GenericEnvironment::Serialize header: six u64 counts and seven u32 fields.
void PutEnvHeader(std::vector<u8>& out, u64 code_size, u64 cbuf_count, u32 local_memory,
                  u32 texture_bound, u32 start_address, u32 stage) {
    Put<u64>(out, code_size);
    Put<u64>(out, 0); // texture types
    Put<u64>(out, 0); // texture pixel formats
    Put<u64>(out, cbuf_count);
    Put<u64>(out, 0); // storage pointer types
    Put<u64>(out, 0); // cbuf replacements
    Put<u32>(out, local_memory);
    Put<u32>(out, texture_bound);
    Put<u32>(out, start_address);
    Put<u32>(out, 0);                                                      // read lowest
    Put<u32>(out, static_cast<u32>(code_size >= 8 ? code_size - 8 : 0));  // read highest
    Put<u32>(out, 1);                                                      // viewport transform state
    Put<u32>(out, stage);
}

} // Anonymous namespace

std::optional<std::string> LowerProgram(std::span<const u8> program, LoweredCode& out) {
    using Shader::Ampere::Instruction;
    out.code.clear();
    out.native_hash = 0;
    out.texture_buffer = ~0U;
    if (program.empty() || program.size() % sizeof(Instruction) ||
        program.size() / sizeof(Instruction) > 65536) {
        return "Native input must contain 1..65536 complete 16-byte instructions";
    }
    std::vector<Instruction> native(program.size() / sizeof(Instruction));
    std::memcpy(native.data(), program.data(), native.size() * sizeof(Instruction));
    Shader::Ampere::ProgramCaptureState capture;
    bool complete = false;
    for (u32 index = 0; index < native.size(); ++index) {
        const auto result = capture.Observe(native[index], index);
        if (result == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
            return "Native allocation has an out-of-range branch target";
        }
        if (result == Shader::Ampere::ProgramCaptureState::Result::Complete) {
            native.resize(index + 1);
            complete = true;
            break;
        }
    }
    if (!complete) {
        return "Native allocation lacks a complete runtime capture";
    }

    // Match GenericEnvironment::CaptureNativeCode for a compute dispatch: store the captured
    // SM86 blob and deterministic discovery hash so warm entries share runtime pipeline keys.
    LoweredNativeCode native_info;
    native_info.native_program_key = Shader::Ampere::NativeProgramKey(native);
    native_info.diagnostic_key = native_info.native_program_key;
    native_info.source_size = native.size() * sizeof(Instruction);
    native_info.native_code = std::move(native);
    native_info.source_header_size = 0;
    native_info.has_native_context = true;
    native_info.native_hash = ComputeNativeDiscoveryHash(native_info);

    const std::vector<u64> blob = PackNativeSm86Blob(native_info, false);
    out.code.resize(blob.size() * sizeof(u64));
    std::memcpy(out.code.data(), blob.data(), out.code.size());
    out.native_hash = native_info.native_hash;
    out.texture_buffer = ~0U;
    return std::nullopt;
}

u64 StageHash(std::span<const u8> code) {
    if (code.empty() || code.size() % sizeof(u64) != 0) {
        return 0;
    }
    std::vector<u64> words(code.size() / sizeof(u64));
    std::memcpy(words.data(), code.data(), words.size() * sizeof(u64));
    LoweredNativeCode unpacked;
    bool has_hle{};
    if (TryUnpackNativeSm86Blob(words, unpacked, has_hle)) {
        return unpacked.native_hash != 0 ? unpacked.native_hash
                                         : ComputeNativeDiscoveryHash(unpacked);
    }
    return NativeShaderHash(words);
}

std::optional<ComputeControl> ReadComputeControl(std::span<const u8> control) {
    if (control.size() < 108) {
        return std::nullopt;
    }
    ComputeControl result;
    for (size_t i = 0; i < 3; ++i) {
        result.workgroup[i] = Get<u32>(control, 88 + i * 4);
    }
    result.shared_memory =
        static_cast<u32>((u64{Get<u32>(control, 100)} + 255) / 256 * 256);
    result.local_memory = Get<u32>(control, 104);
    return result;
}

std::vector<u8> ComputeEntry(const LoweredCode& lowered, const ComputeControl& control,
                             ComputeKey& key) {
    std::vector<u8> out;
    out.reserve(4 + EnvHeaderSize + lowered.code.size() + 16 + key.size());
    Put<u32>(out, 1);
    // Like InitializeNativeCode: the program's own bank when it names one.
    const u32 texture_bound = lowered.texture_buffer != ~0U ? lowered.texture_buffer : 0;
    PutEnvHeader(out, lowered.code.size(), 0, control.local_memory, texture_bound, 80,
                 ComputeStage);
    PutBytes(out, lowered.code);
    for (const u32 size : control.workgroup) {
        Put<u32>(out, size);
    }
    Put<u32>(out, control.shared_memory);
    std::vector<u8> key_bytes;
    const u64 unique_hash =
        lowered.native_hash != 0 ? lowered.native_hash : StageHash(lowered.code);
    Put<u64>(key_bytes, unique_hash);
    Put<u32>(key_bytes, control.shared_memory);
    for (const u32 size : control.workgroup) {
        Put<u32>(key_bytes, size);
    }
    std::memcpy(key.data(), key_bytes.data(), key.size());
    PutBytes(out, key_bytes);
    return out;
}

} // namespace VideoCommon::ShaderWarm
