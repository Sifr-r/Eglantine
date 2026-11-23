// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "common/assert.h"
#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/div_ceil.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include <ranges>
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_override.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/nx2_diag.h"
#include "video_core/mkw_wave_workaround.h"
#include "video_core/shader_environment.h"
#include "video_core/texture_cache/format_lookup_table.h"
#include "video_core/textures/texture.h"

namespace VideoCommon {

constexpr std::array<char, 8> MAGIC_NUMBER{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};

constexpr size_t INST_SIZE = sizeof(u64);
constexpr u64 NATIVE_SM86_SERIALIZED_MAGIC = 0x4544454e534d3836ULL;
constexpr u64 NATIVE_SM86_BLOB_VERSION = 1;
constexpr size_t NATIVE_HEADER_WORDS = sizeof(Shader::ProgramHeader) / sizeof(u64);
constexpr size_t NATIVE_SM86_FOOTER_WORDS = 10;

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

u64 ComputeNativeDiscoveryHash(const LoweredNativeCode& native_info) {
    std::vector<u64> hash_words;
    hash_words.reserve(NATIVE_HEADER_WORDS + 1 + native_info.native_code.size() * 2);
    hash_words.insert(hash_words.end(), native_info.header.begin(), native_info.header.end());
    const u64 flags = (static_cast<u64>(native_info.source_header_size) << 3) |
                      (native_info.geometry ? 1ULL : 0ULL) |
                      (native_info.native_tensor ? 2ULL : 0ULL) |
                      (native_info.native_compact_tensor ? 4ULL : 0ULL);
    hash_words.push_back(flags);
    for (const auto& insn : native_info.native_code) {
        u64 lo{};
        u64 hi{};
        std::memcpy(&lo, &insn.words[0], sizeof(lo));
        std::memcpy(&hi, &insn.words[2], sizeof(hi));
        hash_words.push_back(lo);
        hash_words.push_back(hi);
    }
    return Common::CityHash64(reinterpret_cast<const char*>(hash_words.data()),
                              hash_words.size() * sizeof(u64)) ^
           0x4e58325f4e415456ULL;
}

std::vector<u64> PackNativeSm86Blob(const LoweredNativeCode& native_info,
                                    bool has_hle_engine_state) {
    std::vector<u64> blob;
    blob.reserve(NATIVE_HEADER_WORDS + native_info.native_code.size() * 2 +
                 NATIVE_SM86_FOOTER_WORDS);
    blob.insert(blob.end(), native_info.header.begin(), native_info.header.end());
    for (const auto& insn : native_info.native_code) {
        u64 lo{};
        u64 hi{};
        std::memcpy(&lo, &insn.words[0], sizeof(lo));
        std::memcpy(&hi, &insn.words[2], sizeof(hi));
        blob.push_back(lo);
        blob.push_back(hi);
    }
    const u64 flags = (native_info.geometry ? 1ULL : 0ULL) |
                      (native_info.native_tensor ? (1ULL << 1) : 0ULL) |
                      (native_info.native_compact_tensor ? (1ULL << 2) : 0ULL) |
                      (has_hle_engine_state ? (1ULL << 3) : 0ULL);
    blob.push_back(native_info.native_program_key);
    blob.push_back(native_info.diagnostic_key);
    blob.push_back(static_cast<u64>(native_info.source_size));
    blob.push_back(native_info.native_hash);
    blob.push_back(native_info.program_base);
    blob.push_back(static_cast<u64>(native_info.source_header_size));
    blob.push_back(flags);
    blob.push_back(static_cast<u64>(native_info.native_code.size()));
    blob.push_back(NATIVE_SM86_BLOB_VERSION);
    blob.push_back(NATIVE_SM86_SERIALIZED_MAGIC);
    return blob;
}

bool TryUnpackNativeSm86Blob(std::span<const u64> words, LoweredNativeCode& out,
                             bool& out_has_hle_engine_state) {
    if (words.size() < NATIVE_HEADER_WORDS + NATIVE_SM86_FOOTER_WORDS) {
        return false;
    }
    if (words[words.size() - 1] != NATIVE_SM86_SERIALIZED_MAGIC ||
        words[words.size() - 2] != NATIVE_SM86_BLOB_VERSION) {
        return false;
    }
    const u64 insn_count_u64 = words[words.size() - 3];
    const size_t max_insns = (words.size() - NATIVE_HEADER_WORDS - NATIVE_SM86_FOOTER_WORDS) / 2;
    if (insn_count_u64 > max_insns) {
        return false;
    }
    const size_t insn_count = static_cast<size_t>(insn_count_u64);
    if (words.size() != NATIVE_HEADER_WORDS + insn_count * 2 + NATIVE_SM86_FOOTER_WORDS) {
        return false;
    }
    const size_t footer = NATIVE_HEADER_WORDS + insn_count * 2;
    std::copy_n(words.begin(), NATIVE_HEADER_WORDS, out.header.begin());
    out.native_code.resize(insn_count);
    for (size_t i = 0; i < insn_count; ++i) {
        const u64 lo = words[NATIVE_HEADER_WORDS + i * 2];
        const u64 hi = words[NATIVE_HEADER_WORDS + i * 2 + 1];
        std::memcpy(&out.native_code[i].words[0], &lo, sizeof(lo));
        std::memcpy(&out.native_code[i].words[2], &hi, sizeof(hi));
    }
    out.native_program_key = words[footer + 0];
    out.diagnostic_key = words[footer + 1];
    out.source_size = static_cast<size_t>(words[footer + 2]);
    out.native_hash = words[footer + 3];
    out.program_base = words[footer + 4];
    out.source_header_size = static_cast<u32>(words[footer + 5]);
    const u64 flags = words[footer + 6];
    out.geometry = (flags & 1ULL) != 0;
    out.native_tensor = (flags & (1ULL << 1)) != 0;
    out.native_compact_tensor = (flags & (1ULL << 2)) != 0;
    out_has_hle_engine_state = (flags & (1ULL << 3)) != 0;
    out.has_native_context = true;
    out.lazy = std::make_shared<LoweredNativeCode::LazyLoweringState>();
    return true;
}

namespace {

std::atomic<u64> g_lower_to_maxwell_invocations{0};

void CheckNativeRejectAddress(GPUVAddr program_base) {
    // EDEN_NX2_REJECT=<hex address>,...: treat these programs as rejected (to find which newly
    // lowered program changes a game's behaviour without rebuilding).
    if (static const std::string reject_list = [] {
            const char* text = std::getenv("EDEN_NX2_REJECT");
            return text ? "," + std::string{text} + "," : std::string{};
        }();
        !reject_list.empty() &&
        reject_list.find(fmt::format(",{:x},", program_base)) != std::string::npos) {
        throw Shader::RuntimeError("Native shader at {:#x} rejected by EDEN_NX2_REJECT",
                                   program_base);
    }
}

void CheckNativeRejectHashes(GPUVAddr program_base, std::initializer_list<u64> hashes) {
    // EDEN_NX2_REJECT_HASH=<hash>,...: like EDEN_NX2_REJECT, but by native program key (shader
    // dump file names) or shader hash (frame dump "shaders"), which stay the same between runs.
    if (static const std::string reject_hashes =
            [] {
                const char* text = std::getenv("EDEN_NX2_REJECT_HASH");
                std::string list = text ? "," + std::string{text} + "," : std::string{};
                std::ranges::transform(list, list.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                return list;
            }();
        !reject_hashes.empty()) {
        for (const u64 hash : hashes) {
            if (hash != 0 &&
                reject_hashes.find(fmt::format(",{:016x},", hash)) != std::string::npos) {
                throw Shader::RuntimeError(
                    "Native shader at {:#x} ({:016x}) rejected by EDEN_NX2_REJECT_HASH",
                    program_base, hash);
            }
        }
    }
    // The same list, changed at runtime by the debug server (docs/nx2/debug-mcp.md).
    if (VideoCore::Nx2Debug::AnyRejected()) {
        for (const u64 hash : hashes) {
            if (hash != 0 && VideoCore::Nx2Debug::IsRejected(hash)) {
                throw Shader::RuntimeError(
                    "Native shader at {:#x} ({:016x}) rejected by the debug server", program_base,
                    hash);
            }
        }
    }
}

void EnsureLoweredNativeCodeImpl(const std::shared_ptr<const LoweredNativeCode>& native_info,
                                 Tegra::MemoryManager* gpu_memory) {
    if (!native_info || !native_info->code.empty() || !native_info->lazy) {
        return;
    }
    auto& state = *native_info->lazy;
    if (state.attempted.load(std::memory_order_acquire)) {
        if (!state.succeeded) {
            std::rethrow_exception(state.error);
        }
        return;
    }

    std::scoped_lock lock{state.mutex};
    if (state.attempted.load(std::memory_order_relaxed)) {
        if (!state.succeeded) {
            std::rethrow_exception(state.error);
        }
        return;
    }

    try {
        std::vector<Shader::Ampere::Instruction> native = native_info->native_code;
        g_lower_to_maxwell_invocations.fetch_add(1, std::memory_order_relaxed);
        auto lowered = Shader::Ampere::LowerToMaxwell(
            native, {.geometry = native_info->geometry,
                     .tensor = native_info->native_tensor,
                     .compact_tensor = native_info->native_compact_tensor,
                     .native_code_address =
                         native_info->program_base + native_info->source_header_size});
        if (!lowered) {
            if (const char* directory = std::getenv("EDEN_NATIVE_SHADER_OVERRIDE_DIR")) {
                const auto key = fmt::format("{:016x}", native_info->native_program_key);
                const auto folder = std::filesystem::path{directory};
                std::filesystem::create_directories(folder);
                const auto source_path = folder / (key + ".source.bin");
                const auto override_path = folder / (key + ".override.bin");
                {
                    std::ofstream dump(source_path, std::ios::binary);
                    dump.write(reinterpret_cast<const char*>(native.data()),
                               native.size() * sizeof(Shader::Ampere::Instruction));
                }
                if (gpu_memory != nullptr) {
                    // Retain mapped bytes beyond the first EXIT for investigating
                    // native CALL targets in compiler library subroutines.
                    std::ofstream mapped(folder / (key + ".mapped.bin"), std::ios::binary);
                    std::array<u8, 0x1000> block{};
                    for (u32 offset = 0; offset < 0x10000; offset += 0x1000) {
                        if (!gpu_memory->IsFullyMappedRange(native_info->program_base + offset,
                                                            block.size())) {
                            break;
                        }
                        gpu_memory->ReadBlockUnsafe(native_info->program_base + offset, block.data(),
                                                    block.size());
                        mapped.write(reinterpret_cast<const char*>(block.data()), block.size());
                    }
                }
                LOG_WARNING(Shader, "Native shader paused: key={}, offset={:#x}, reason={}, source={}",
                            key, lowered.error_offset, lowered.error, source_path.string());
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes{30};
                while ((gpu_memory == nullptr || !gpu_memory->IsShuttingDown()) &&
                       std::chrono::steady_clock::now() < deadline) {
                    if (std::filesystem::exists(override_path)) {
                        std::ifstream file(override_path, std::ios::binary | std::ios::ate);
                        const auto length = file.tellg();
                        if (length <= 0 || length > 16 * 1024 * 1024) {
                            throw Shader::RuntimeError("Invalid native shader override size");
                        }
                        std::vector<u8> bytes(static_cast<size_t>(length));
                        file.seekg(0);
                        file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
                        if (!file) {
                            throw Shader::RuntimeError("Native shader override read failed");
                        }
                        const u32 source_bytes = Shader::Ampere::ProgramOverrideSourceSize(bytes);
                        if ((source_bytes % sizeof(Shader::Ampere::Instruction)) != 0 ||
                            source_bytes >
                                Shader::Ampere::ProgramCaptureState::MaximumInstructions *
                                    sizeof(Shader::Ampere::Instruction)) {
                            throw Shader::RuntimeError("Invalid native shader override source size {}",
                                                       source_bytes);
                        }
                        if (source_bytes >
                            native.size() * sizeof(Shader::Ampere::Instruction)) {
                            if (gpu_memory == nullptr ||
                                !gpu_memory->IsFullyMappedRange(
                                    native_info->program_base + native_info->source_header_size,
                                    source_bytes)) {
                                throw Shader::RuntimeError(
                                    "Unmapped extended native shader source");
                            }
                            native.resize(source_bytes / sizeof(Shader::Ampere::Instruction));
                            gpu_memory->ReadBlockUnsafe(
                                native_info->program_base + native_info->source_header_size,
                                native.data(), source_bytes);
                        }
                        lowered = Shader::Ampere::DecodeProgramOverride(bytes, native);
                        if (!lowered) {
                            throw Shader::RuntimeError("Native shader override rejected: {}",
                                                       lowered.error);
                        }
                        LOG_WARNING(Shader, "Native shader resumed: key={}, instructions={}", key,
                                    lowered.instructions.size());
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{100});
                }
            }
        }
        if (!lowered) {
            const size_t failed = lowered.error_offset / sizeof(Shader::Ampere::Instruction);
            const auto words =
                failed < native.size() ? native[failed].words : std::array<u32, 4>{};
            throw Shader::RuntimeError(
                "Native shader at {:#x}, instruction {:#x}: {} [op {:03x} {:08x} {:08x} {:08x} "
                "{:08x}]",
                native_info->program_base, lowered.error_offset, lowered.error, words[0] & 0xfff,
                words[0], words[1], words[2], words[3]);
        }

        state.texture_bound = lowered.required_texture_buffer;
        state.source_size = native_info->source_header_size + lowered.source_size;
        state.code = LayOutLoweredNativeCode(native_info->header, native, lowered);

        const u64 shader_hash = NativeShaderHash(state.code);
        if (VideoCommon::Nx2Diag::Active()) {
            VideoCommon::Nx2Diag::Diagnostics::Get().RegisterProgram(
                native_info->program_base, native_info->native_program_key, shader_hash);
        }
        CheckNativeRejectHashes(native_info->program_base,
                                {native_info->native_program_key, shader_hash});

        state.succeeded = true;
        LOG_DEBUG(Shader, "Lazily lowered native shader at {:#x}: {} source bytes, {} instructions",
                  native_info->program_base, state.source_size, lowered.instructions.size());
        state.attempted.store(true, std::memory_order_release);
    } catch (...) {
        state.error = std::current_exception();
        state.succeeded = false;
        state.attempted.store(true, std::memory_order_release);
        std::rethrow_exception(state.error);
    }
}

} // namespace

void ResetLowerToMaxwellInvocationCount() noexcept {
    g_lower_to_maxwell_invocations.store(0, std::memory_order_relaxed);
}

u64 LowerToMaxwellInvocationCount() noexcept {
    return g_lower_to_maxwell_invocations.load(std::memory_order_relaxed);
}

// A version tag in the hash isolates this experimental ABI.
u64 NativeShaderHash(std::span<const u64> code) {
    return Common::CityHash64(reinterpret_cast<const char*>(code.data()),
                              code.size() * sizeof(u64)) ^
           0x4f554e4345000002ULL;
}

std::vector<u64> LayOutLoweredNativeCode(std::span<const u64> header,
                                         std::span<const Shader::Ampere::Instruction> native,
                                         Shader::Ampere::LoweredProgram& lowered) {
    VideoCommon::ApplyMkwZeroDepthFallback(native, lowered);
    std::vector<u64> code(header.begin(), header.end());
    for (const u64 instruction : lowered.instructions) {
        if ((code.size() * sizeof(u64)) % 32 == 0) {
            code.push_back(0); // Maxwell scheduling word, absent from native instructions.
        }
        code.push_back(instruction);
    }
    // Keep the original padding ABI, then retain the native address map as
    // unreachable data. Existing disk-cache serialization carries it with code.
    code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(code, lowered.source_instruction_offsets);
    return code;
}

static u64 MakeCbufKey(u32 index, u32 offset) {
    return (static_cast<u64>(index) << 32) | offset;
}

// Pointer-table entries share the classification map with plain constant buffer pointers.
static u64 MakeIndirectCbufKey(u32 index, u32 offset, u32 indirect) {
    return (u64{1} << 63) | (static_cast<u64>(indirect & 0x3fffff) << 40) |
           MakeCbufKey(index, offset);
}

// Constant buffer slots that held a null pointer when the program was translated and were
// therefore not bound as storage buffers. A later non-null value makes the translation stale.
static std::vector<std::pair<u32, u32>> NullStoragePointerSlots(
    const ::Common::unordered_map<u64, u32>& cbuf_values,
    const ::Common::unordered_map<u64, u8>& storage_pointer_types) {
    std::vector<std::pair<u32, u32>> slots;
    for (const auto& [key, type] : storage_pointer_types) {
        if (type != 0 || (key >> 63) != 0) {
            continue;
        }
        const auto low = cbuf_values.find(key);
        const auto high = cbuf_values.find(key + 4);
        if (low != cbuf_values.end() && high != cbuf_values.end() && low->second == 0 &&
            high->second == 0) {
            slots.emplace_back(static_cast<u32>(key >> 32), static_cast<u32>(key));
        }
    }
    return slots;
}

static std::vector<u64> MappedStoragePointerKeys(
    const ::Common::unordered_map<u64, u8>& storage_pointer_types) {
    std::vector<u64> keys;
    for (const auto& [key, type] : storage_pointer_types) {
        if (type != 0) {
            keys.push_back(key);
        }
    }
    std::ranges::sort(keys);
    return keys;
}

static Shader::TextureType ConvertTextureType(const Tegra::Texture::TICEntry& entry) {
    switch (entry.texture_type) {
    case Tegra::Texture::TextureType::Texture1D:
        return Shader::TextureType::Color1D;
    case Tegra::Texture::TextureType::Texture2D:
    case Tegra::Texture::TextureType::Texture2DNoMipmap:
        return entry.normalized_coords ? Shader::TextureType::Color2D
                                       : Shader::TextureType::Color2DRect;
    case Tegra::Texture::TextureType::Texture3D:
        return Shader::TextureType::Color3D;
    case Tegra::Texture::TextureType::TextureCubemap:
        return Shader::TextureType::ColorCube;
    case Tegra::Texture::TextureType::Texture1DArray:
        return Shader::TextureType::ColorArray1D;
    case Tegra::Texture::TextureType::Texture2DArray:
        return Shader::TextureType::ColorArray2D;
    case Tegra::Texture::TextureType::Texture1DBuffer:
        return Shader::TextureType::Buffer;
    case Tegra::Texture::TextureType::TextureCubeArray:
        return Shader::TextureType::ColorArrayCube;
    default:
        LOG_ERROR(Shader, "Invalid texture_type={}. Falling back to texture_type={}", static_cast<int>(entry.texture_type.Value()), Shader::TextureType::Color2D);
        return Shader::TextureType::Color2D;
    }
}

static Shader::TexturePixelFormat ConvertTexturePixelFormat(const Tegra::Texture::TICEntry& entry) {
    return static_cast<Shader::TexturePixelFormat>(
        PixelFormatFromTextureInfo(entry.format, entry.r_type, entry.g_type, entry.b_type,
                                   entry.a_type, entry.srgb_conversion));
}

static std::string_view StageToPrefix(Shader::Stage stage) {
    switch (stage) {
    case Shader::Stage::VertexB:
        return "vs";
    case Shader::Stage::TessellationControl:
        return "tc";
    case Shader::Stage::TessellationEval:
        return "te";
    case Shader::Stage::Geometry:
        return "gs";
    case Shader::Stage::Fragment:
        return "fs";
    case Shader::Stage::Compute:
        return "cs";
    case Shader::Stage::VertexA:
        return "va";
    default:
        return "uk";
    }
}

static void DumpImpl(u64 /*pipeline_hash*/, u64 shader_hash, std::span<const u64> code,
                     [[maybe_unused]] u32 read_highest, [[maybe_unused]] u32 read_lowest,
                     u32 initial_offset, Shader::Stage stage) {
    const auto dump_dir{Common::FS::GetEdenPath(Common::FS::EdenPath::DumpDir)};
    if (!Common::FS::CreateDir(dump_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create dump directory");
        return;
    }
    const auto prefix = StageToPrefix(stage);
    const auto name{dump_dir /
                    fmt::format("{:016x}_{:016x}_{}.ash",
                                Settings::GetCurrentProgramID(), shader_hash, prefix)};
    std::fstream shader_file(name, std::ios::out | std::ios::binary);
    ASSERT(initial_offset % sizeof(u64) == 0);
    const size_t jump_index = initial_offset / sizeof(u64);
    const size_t code_size = code.size_bytes() - initial_offset;
    shader_file.write(reinterpret_cast<const char*>(&code[jump_index]), code_size);

    // + 1 instruction, due to the fact that we skip the final self branch instruction in the code,
    // but we need to consider it for padding, otherwise nvdisasm rages.
    const size_t padding_needed = (32 - ((code_size + INST_SIZE) % 32)) % 32;
    for (size_t i = 0; i < INST_SIZE + padding_needed; i++) {
        shader_file.put(0);
    }
}

GenericEnvironment::GenericEnvironment(Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                       u32 start_address_)
    : gpu_memory{&gpu_memory_}, program_base{program_base_} {
    start_address = start_address_;
}

GenericEnvironment::~GenericEnvironment() = default;

u32 GenericEnvironment::TextureBoundBuffer() const {
    if (native_source_size != 0 && code.empty() && lowered_native) {
        if (const u32 tb = lowered_native->EffectiveTextureBound(); tb != ~0U) {
            return tb;
        }
    }
    return texture_bound;
}

u32 GenericEnvironment::LocalMemorySize() const {
    return local_memory_size;
}

u32 GenericEnvironment::SharedMemorySize() const {
    return shared_memory_size;
}

std::array<u32, 3> GenericEnvironment::WorkgroupSize() const {
    return workgroup_size;
}

u64 GenericEnvironment::ReadInstruction(u32 address) {
    read_lowest = (std::min)(read_lowest, address);
    read_highest = (std::max)(read_highest, address);

    if (native_source_size != 0 && code.empty()) {
        EnsureLoweredNative();
    }
    if (address >= cached_lowest && address < cached_highest) {
        return code[(address - cached_lowest) / INST_SIZE];
    }
    if (native_source_size) {
        throw Shader::RuntimeError("Lowered native instruction address out of range: {:#x}", address);
    }
    has_unbound_instructions = true;
    return gpu_memory->Read<u64>(program_base + address);
}

u32 GenericEnvironment::TranslateProgramAddress(u32 address) const {
    if (!code.empty()) {
        if (const auto translated = Shader::Ampere::NativeProgramAddress(code, address)) {
            return *translated;
        }
    } else if (native_source_size != 0 && lowered_native) {
        EnsureLoweredNativeCodeImpl(lowered_native, gpu_memory);
        if (const auto translated =
                Shader::Ampere::NativeProgramAddress(lowered_native->LoweredWords(), address)) {
            return *translated;
        }
    }
    if (native_source_size || Shader::Ampere::HasNativeAddressMap(code)) {
        throw Shader::RuntimeError("Invalid native indirect branch address {:#x}", address);
    }
    return address;
}

bool GenericEnvironment::IsNativeProgram() const {
    return native_source_size != 0 || Shader::Ampere::HasNativeAddressMap(code) ||
           (lowered_native && !lowered_native->native_code.empty());
}

bool GenericEnvironment::IsCbufStorageBufferPointer(u32 index, u32 offset) {
    const u64 key = MakeCbufKey(index, offset);
    if (const auto it = storage_pointer_types.find(key); it != storage_pointer_types.end()) {
        return it->second != 0;
    }
    const u64 address = u64{ReadCbufValue(index, offset)} |
                        (u64{ReadCbufValue(index, offset + 4)} << 32);
    const bool mapped = address != 0 && gpu_memory->IsWithinGPUAddressRange(address) &&
                        gpu_memory->IsFullyMappedRange(address, 1);
    storage_pointer_types.emplace(key, static_cast<u8>(mapped));
    return mapped;
}

bool GenericEnvironment::IsCbufIndirectStorageBufferPointer(u32 index, u32 offset, u32 indirect) {
    const u64 key = MakeIndirectCbufKey(index, offset, indirect);
    if (const auto it = storage_pointer_types.find(key); it != storage_pointer_types.end()) {
        return it->second != 0;
    }
    bool mapped = false;
    if (IsCbufStorageBufferPointer(index, offset)) {
        const u64 holder =
            u64{ReadCbufValue(index, offset)} | (u64{ReadCbufValue(index, offset + 4)} << 32);
        if (gpu_memory->IsFullyMappedRange(holder + indirect, sizeof(u64))) {
            const u64 address = gpu_memory->Read<u64>(holder + indirect);
            mapped = address != 0 && gpu_memory->IsWithinGPUAddressRange(address) &&
                     gpu_memory->IsFullyMappedRange(address, 1);
        }
    }
    storage_pointer_types.emplace(key, static_cast<u8>(mapped));
    return mapped;
}

std::vector<std::pair<u32, u32>> GenericEnvironment::NullStoragePointerSlots() const {
    return VideoCommon::NullStoragePointerSlots(cbuf_values, storage_pointer_types);
}

std::vector<u64> GenericEnvironment::MappedStoragePointerKeys() const {
    return VideoCommon::MappedStoragePointerKeys(storage_pointer_types);
}

void GenericEnvironment::KeepStoragePointers(std::span<const u64> keys) {
    for (const u64 key : keys) {
        storage_pointer_types.insert_or_assign(key, u8{1});
    }
}

std::optional<u64> GenericEnvironment::Analyze() {
    if (native_source_size) {
        if (lowered_native && lowered_native->native_hash != 0) {
            return lowered_native->native_hash;
        }
        // Fallback if a manually constructed LoweredNativeCode supplied lowered words directly.
        return NativeShaderHash(code);
    }
    const std::optional<u64> size{TryFindSize()};
    if (!size) {
        return std::nullopt;
    }
    cached_lowest = start_address;
    cached_highest = start_address + static_cast<u32>(*size);
    return Common::CityHash64(reinterpret_cast<const char*>(code.data()), *size);
}

void GenericEnvironment::SetCachedSize(size_t size_bytes) {
    if (native_source_size) {
        if (size_bytes != native_source_size && size_bytes != CachedSizeBytes()) {
            throw Shader::RuntimeError("Native shader changed while constructing pipeline");
        }
        return;
    }
    cached_lowest = start_address;
    cached_highest = start_address + static_cast<u32>(size_bytes);
    code.resize(CachedSizeWords());
    gpu_memory->ReadBlock(program_base + cached_lowest, code.data(), code.size() * sizeof(u64));
}

size_t GenericEnvironment::CachedSizeWords() const noexcept {
    return CachedSizeBytes() / INST_SIZE;
}

size_t GenericEnvironment::CachedSizeBytes() const noexcept {
    if (native_source_size != 0 && code.empty()) {
        return native_source_size;
    }
    return static_cast<size_t>(cached_highest) - cached_lowest + INST_SIZE;
}

size_t GenericEnvironment::ReadSizeBytes() const noexcept {
    return read_highest - read_lowest + INST_SIZE;
}

bool GenericEnvironment::CanBeSerialized() const noexcept {
    return !has_unbound_instructions;
}

u64 GenericEnvironment::CalculateHash() const {
    const size_t size{ReadSizeBytes()};
    const auto data{std::make_unique<char[]>(size)};
    gpu_memory->ReadBlock(program_base + read_lowest, data.get(), size);
    return Common::CityHash64(data.get(), size);
}

void GenericEnvironment::Dump(u64 pipeline_hash, u64 shader_hash) {
    if (native_source_size != 0 && code.empty()) {
        if (!lowered_native || !lowered_native->HasLoweredCode()) {
            return;
        }
        const auto words = lowered_native->LoweredWords();
        DumpImpl(pipeline_hash, shader_hash, words, read_highest, read_lowest, initial_offset,
                 stage);
        return;
    }
    DumpImpl(pipeline_hash, shader_hash, code, read_highest, read_lowest, initial_offset, stage);
}

void GenericEnvironment::Serialize(std::ofstream& file, bool with_mesh_state) const {
    std::vector<u64> unlowered_blob;
    std::span<const u64> words_to_write{code};
    u64 code_size{static_cast<u64>(CachedSizeBytes())};
    u32 write_texture_bound{texture_bound};
    u32 write_cached_lowest{cached_lowest};
    u32 write_cached_highest{cached_highest};
    if (native_source_size != 0 && code.empty()) {
        if (lowered_native && !lowered_native->native_code.empty()) {
            // Keep the immutable SM86 blob format even if another thread lazily lowered shared
            // LoweredNativeCode before serialization runs.
            unlowered_blob = PackNativeSm86Blob(*lowered_native, has_hle_engine_state);
            words_to_write = unlowered_blob;
        } else if (lowered_native && lowered_native->HasLoweredCode()) {
            words_to_write = lowered_native->LoweredWords();
            if (const u32 tb = lowered_native->EffectiveTextureBound(); tb != ~0U) {
                write_texture_bound = tb;
            }
        }
        code_size = static_cast<u64>(words_to_write.size() * sizeof(u64));
        write_cached_lowest = 0;
        write_cached_highest =
            words_to_write.empty()
                ? 0U
                : static_cast<u32>((words_to_write.size() - 1) * sizeof(u64));
    }

    const u64 num_texture_types{static_cast<u64>(texture_types.size())};
    const u64 num_texture_pixel_formats{static_cast<u64>(texture_pixel_formats.size())};
    const u64 num_cbuf_values{static_cast<u64>(cbuf_values.size())};
    const u64 num_storage_pointer_types{static_cast<u64>(storage_pointer_types.size())};
    const u64 num_cbuf_replacement_values{static_cast<u64>(cbuf_replacements.size())};

    file.write(reinterpret_cast<const char*>(&code_size), sizeof(code_size))
        .write(reinterpret_cast<const char*>(&num_texture_types), sizeof(num_texture_types))
        .write(reinterpret_cast<const char*>(&num_texture_pixel_formats),
               sizeof(num_texture_pixel_formats))
        .write(reinterpret_cast<const char*>(&num_cbuf_values), sizeof(num_cbuf_values))
        .write(reinterpret_cast<const char*>(&num_storage_pointer_types),
               sizeof(num_storage_pointer_types))
        .write(reinterpret_cast<const char*>(&num_cbuf_replacement_values),
               sizeof(num_cbuf_replacement_values))
        .write(reinterpret_cast<const char*>(&local_memory_size), sizeof(local_memory_size))
        .write(reinterpret_cast<const char*>(&write_texture_bound), sizeof(write_texture_bound))
        .write(reinterpret_cast<const char*>(&start_address), sizeof(start_address))
        .write(reinterpret_cast<const char*>(&write_cached_lowest), sizeof(write_cached_lowest))
        .write(reinterpret_cast<const char*>(&write_cached_highest), sizeof(write_cached_highest))
        .write(reinterpret_cast<const char*>(&viewport_transform_state),
               sizeof(viewport_transform_state))
        .write(reinterpret_cast<const char*>(&stage), sizeof(stage))
        .write(words_to_write.empty() ? "" : reinterpret_cast<const char*>(words_to_write.data()),
               code_size);
    for (const auto& [key, type] : texture_types) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    for (const auto& [key, format] : texture_pixel_formats) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&format), sizeof(format));
    }
    for (const auto& [key, type] : cbuf_values) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    for (const auto& [key, type] : storage_pointer_types) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    for (const auto& [key, type] : cbuf_replacements) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    if (stage == Shader::Stage::Compute) {
        file.write(reinterpret_cast<const char*>(&workgroup_size), sizeof(workgroup_size))
            .write(reinterpret_cast<const char*>(&shared_memory_size), sizeof(shared_memory_size));
    } else {
        file.write(reinterpret_cast<const char*>(&sph), sizeof(sph));
        if (stage == Shader::Stage::Geometry) {
            file.write(reinterpret_cast<const char*>(&gp_passthrough_mask),
                       sizeof(gp_passthrough_mask));
        }
        if (with_mesh_state && (stage == Shader::Stage::Task || stage == Shader::Stage::Mesh)) {
            file.write(reinterpret_cast<const char*>(&mesh_registers), sizeof(mesh_registers))
                .write(reinterpret_cast<const char*>(&mesh_primitive_header),
                       sizeof(mesh_primitive_header));
        }
    }
}

std::optional<u64> GenericEnvironment::TryFindSize() {
    static constexpr size_t BLOCK_SIZE = 0x1000;
    static constexpr size_t MAXIMUM_SIZE = 0x100000;

    static constexpr u64 SELF_BRANCH_A = 0xE2400FFFFF87000FULL;
    static constexpr u64 SELF_BRANCH_B = 0xE2400FFFFF07000FULL;

    static constexpr u64 EXIT_VALUE = 0xE30000000007000FULL;

    code.resize(MAXIMUM_SIZE / INST_SIZE);

    GPUVAddr guest_addr{program_base + start_address};
    size_t offset{0};
    size_t size{BLOCK_SIZE};
    while (size <= MAXIMUM_SIZE) {
        u64* const data = code.data() + offset / INST_SIZE;
        gpu_memory->ReadBlock(guest_addr, data, BLOCK_SIZE);
        for (size_t index = 0; index < BLOCK_SIZE; index += INST_SIZE) {
            const u64 inst = data[index / INST_SIZE];
            if (inst == SELF_BRANCH_A || inst == SELF_BRANCH_B) {
                return offset + index;
            }
            if (!is_proprietary_driver && inst == EXIT_VALUE) {
                return offset + index + INST_SIZE;
            }
        }
        guest_addr += BLOCK_SIZE;
        size += BLOCK_SIZE;
        offset += BLOCK_SIZE;
    }
    return std::nullopt;
}

Tegra::Texture::TICEntry GenericEnvironment::ReadTextureInfo(GPUVAddr tic_addr, u32 tic_limit,
                                                             bool via_header_index, u32 raw) {
    const auto handle{Tegra::Texture::TexturePair(raw, via_header_index)};
    const u32 index{Tegra::Texture::TextureHeaderIndex(handle.first, tic_limit,
                                                       gpu_memory->UsesNativeTiling())};
    if (index > tic_limit) {
        return {};
    }
    const GPUVAddr descriptor_addr{tic_addr + index * sizeof(Tegra::Texture::TICEntry)};
    Tegra::Texture::TICEntry entry{};
    gpu_memory->ReadBlock(descriptor_addr, &entry, sizeof(entry));
    return entry;
}

namespace {

// Reads a native program a 4 KiB page at a time instead of one 16-byte instruction at a time.
// Each page is read with ReadBlock, so GPU-written code is flushed first, once per page instead
// of once per instruction. An instruction that is not inside a fully mapped page is left to the
// caller.
class NativeInstructionReader {
public:
    using Instruction = Shader::Ampere::Instruction;
    static constexpr GPUVAddr PageSize = 0x1000;

    explicit NativeInstructionReader(Tegra::MemoryManager& gpu_memory_)
        : gpu_memory{gpu_memory_} {}

    /// Copies the instruction at `address` into `out`, loading its page first if needed.
    /// Returns false when the instruction is not inside a fully mapped page.
    bool TryRead(GPUVAddr address, Instruction& out) {
        if (!Holds(address)) {
            LoadFrom(address);
        }
        if (!Holds(address)) {
            return false;
        }
        out = page[(address - page_begin) / sizeof(Instruction)];
        return true;
    }

private:
    bool Holds(GPUVAddr address) const {
        return address >= page_begin && address + sizeof(Instruction) <= page_end;
    }

    // Loads from `address` up to the next page boundary; loads nothing if that range is not
    // fully mapped.
    void LoadFrom(GPUVAddr address) {
        const GPUVAddr next_page = (address | (PageSize - 1)) + 1;
        const GPUVAddr length = next_page - address;
        page_begin = address;
        page_end = address;
        if (length >= sizeof(Instruction) && gpu_memory.IsFullyMappedRange(address, length)) {
            gpu_memory.ReadBlock(address, page.data(), length);
            page_end = next_page;
        }
    }

    Tegra::MemoryManager& gpu_memory;
    std::array<Instruction, PageSize / sizeof(Instruction)> page;
    GPUVAddr page_begin{};
    GPUVAddr page_end{};
};

} // Anonymous namespace

bool GenericEnvironment::HasLoweredNativeCode() const noexcept {
    if (native_source_size == 0) {
        return false;
    }
    return !code.empty() || (lowered_native && lowered_native->HasLoweredCode());
}

void GenericEnvironment::EnsureLoweredNative() {
    if (native_source_size == 0 || !code.empty()) {
        return;
    }
    if (!lowered_native) {
        throw Shader::RuntimeError("Missing native shader state at {:#x}", program_base);
    }
    EnsureLoweredNativeCodeImpl(lowered_native, gpu_memory);
    const auto words = lowered_native->LoweredWords();
    if (words.empty()) {
        throw Shader::RuntimeError("Lowered native shader is empty at {:#x}", program_base);
    }
    code.assign(words.begin(), words.end());
    cached_lowest = 0;
    cached_highest = static_cast<u32>((code.size() - 1) * sizeof(u64));
    if (const u32 tb = lowered_native->EffectiveTextureBound(); tb != ~0U) {
        texture_bound = tb;
    }
}

// A native program is captured once when the shader cache first meets it, and lowered lazily to
// Maxwell code only when a fallback consumer requests lowered instructions.
void GenericEnvironment::InitializeNativeCode(std::shared_ptr<const LoweredNativeCode> lowered,
                                              u32 source_header_size, bool geometry) {
    if (!lowered) {
        lowered = CaptureNativeCode(source_header_size, geometry);
    }
    native_source_size = lowered->source_size;
    if (const auto words = lowered->LoweredWords(); !words.empty()) {
        code.assign(words.begin(), words.end());
        cached_lowest = 0;
        cached_highest = static_cast<u32>((code.size() - 1) * sizeof(u64));
        if (const u32 tb = lowered->EffectiveTextureBound(); tb != ~0U) {
            // Native TEX carries its cbuf index explicitly; TEXS obtains it from the environment.
            texture_bound = tb;
        }
    } else {
        code.clear();
        cached_lowest = 0;
        cached_highest = 0;
    }
    lowered_native = std::move(lowered);
}

std::vector<Shader::Ampere::Instruction> GenericEnvironment::CaptureNativeInstructions(
    u32 source_header_size) const {
    using Shader::Ampere::Instruction;
    const u32 native_header_size = source_header_size;
    constexpr u32 maximum_instructions = Shader::Ampere::ProgramCaptureState::MaximumInstructions;
    std::vector<Instruction> native;
    Shader::Ampere::ProgramCaptureState capture;
    NativeInstructionReader reader{*gpu_memory};
    for (u32 index = 0; index < maximum_instructions; ++index) {
        const GPUVAddr address = program_base + native_header_size + index * sizeof(Instruction);
        Instruction instruction;
        if (!reader.TryRead(address, instruction)) {
            // Not inside a fully mapped page: check and read this one instruction on its own.
            if (!gpu_memory->IsFullyMappedRange(address, sizeof(Instruction))) {
                // The program may end before a branch target past its last EXIT (the capture
                // can't tell); stop at the mapping's end and let the lowering judge what was
                // captured.
                if (!native.empty()) break;
                throw Shader::RuntimeError("Unmapped native shader instruction at {:#x}", address);
            }
            gpu_memory->ReadBlock(address, &instruction, sizeof(instruction));
        }
        native.push_back(instruction);
        const auto status = capture.Observe(instruction, index);
        if (status == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
            if (const char* dir = std::getenv("EDEN_NX2_SHADER_DUMP")) {
                const auto path = fmt::format("{}/rejected-{:x}.bin", dir, program_base + native_header_size);
                if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(native.data(), sizeof(Instruction), std::min<size_t>(native.size(), 4096), file);
                    std::fclose(file);
                }
            }
            throw Shader::RuntimeError("Native shader branch outside capture bounds at {:#x} +{:#x} [{:08x} {:08x} {:08x} {:08x}]",
                                       program_base + native_header_size, index * 16,
                                       instruction.words[0], instruction.words[1],
                                       instruction.words[2], instruction.words[3]);
        }
        if (status == Shader::Ampere::ProgramCaptureState::Result::Complete) break;
    }
    return native;
}

void GenericEnvironment::PreprobeNativeCbuf([[maybe_unused]] u32 cbuf_index,
                                            [[maybe_unused]] u32 cbuf_offset,
                                            [[maybe_unused]] bool probe_pointer_slot) {}

void GenericEnvironment::PreprobeNativeTexture([[maybe_unused]] u32 cbuf_index,
                                               [[maybe_unused]] u32 cbuf_offset) {}

void GenericEnvironment::TryPreprobeTextureFromPool(GPUVAddr tic_addr, u32 tic_limit,
                                                    bool via_header_index, u32 cbuf_index,
                                                    u32 cbuf_offset) {
    if (gpu_memory == nullptr) {
        return;
    }
    const auto it = cbuf_values.find(MakeCbufKey(cbuf_index, cbuf_offset));
    if (it == cbuf_values.end()) {
        return;
    }
    const u32 handle = it->second;
    const auto pair{Tegra::Texture::TexturePair(handle, via_header_index)};
    const u32 index{Tegra::Texture::TextureHeaderIndex(pair.first, tic_limit,
                                                       gpu_memory->UsesNativeTiling())};
    if (index > tic_limit) {
        return;
    }
    Tegra::Texture::TICEntry entry{};
    const GPUVAddr desc_addr{tic_addr + index * sizeof(entry)};
    if (!gpu_memory->IsFullyMappedRange(desc_addr, sizeof(entry))) {
        return;
    }
    gpu_memory->ReadBlockUnsafe(desc_addr, &entry, sizeof(entry));
    if (static_cast<u32>(entry.texture_type.Value()) >
        static_cast<u32>(Tegra::Texture::TextureType::TextureCubeArray)) {
        return;
    }
    static_cast<void>(ReadTextureType(handle));
    static_cast<void>(ReadTexturePixelFormat(handle));
}

NativeShaderSnapshot GenericEnvironment::BuildNativeSnapshot() {
    NativeShaderSnapshot snap;
    std::memcpy(snap.header.data(), &sph, sizeof(sph));
    if (lowered_native && !lowered_native->native_code.empty()) {
        snap.native_code = lowered_native->native_code;
        snap.program_key = lowered_native->native_program_key;
    } else if (gpu_memory != nullptr) {
        const u32 header_size = stage == Shader::Stage::Compute ? 0U : 128U;
        snap.native_code = CaptureNativeInstructions(header_size);
    }
    if (snap.program_key == 0 && !snap.native_code.empty()) {
        snap.program_key = Shader::Ampere::NativeProgramKey(snap.native_code);
    }
    snap.stage = stage;
    snap.local_memory_size = local_memory_size;
    snap.shared_memory_size = shared_memory_size;
    snap.workgroup_size = workgroup_size;
    snap.texture_bound = TextureBoundBuffer();
    snap.viewport_transform_state = ReadViewportTransformState();
    snap.native_tensor = native_tensor;
    snap.native_compact_tensor = native_compact_tensor;
    snap.has_hle_engine_state = has_hle_engine_state;
    snap.mesh_registers = mesh_registers;
    snap.mesh_primitive_header = mesh_primitive_header;

    for (const auto& insn : snap.native_code) {
        if (insn.Bits(12, 4) == 15U) {
            continue;
        }
        const u32 op = insn.Bits(0, 12);
        const auto normalize_bank = [](u32 raw_bank) -> u32 {
            return raw_bank == 24U ? 17U : raw_bank;
        };
        if ((op == 0xab9U && insn.Bits(24, 8) == 63U) ||
            (op == 0xb82U && insn.Bits(24, 8) == 255U)) {
            const u32 bank = normalize_bank(insn.Bits(54, 5));
            const u32 offset = insn.Bits(38, 16) & ~3U;
            const u32 width = insn.Bits(73, 3);
            if (bank < Tegra::Engines::Maxwell3D::Regs::MaxConstBuffers) {
                PreprobeNativeCbuf(bank, offset, width == 5U && (offset % 8U) == 0U);
                if (width == 5U) {
                    PreprobeNativeCbuf(bank, offset + 4U, false);
                }
            }
            continue;
        }
        const u32 form = op >> 9;
        const bool is_tex_or_mem = (op & 0x0f0U) == 0x060U || (op & 0x0f0U) == 0x080U ||
                                   (op & 0x0f0U) == 0x090U;
        if (!is_tex_or_mem && (form == 3U || form == 5U) && insn.Bits(91, 1) == 0U) {
            const u32 bank = normalize_bank(insn.Bits(54, 5));
            const u32 offset = insn.Bits(38, 16);
            if (bank < Tegra::Engines::Maxwell3D::Regs::MaxConstBuffers && (offset % 4U) == 0U) {
                PreprobeNativeCbuf(bank, offset, false);
            }
            continue;
        }
        if (op == 0xb60U || op == 0xb63U || op == 0xb66U || op == 0xb69U || op == 0xb6cU ||
            op == 0xb6fU) {
            const u32 bank = normalize_bank(insn.Bits(54, 5));
            const u32 offset = insn.Bits(40, 14) * 4U;
            if (bank < Tegra::Engines::Maxwell3D::Regs::MaxConstBuffers) {
                PreprobeNativeCbuf(bank, offset, false);
                PreprobeNativeTexture(bank, offset);
            }
        } else if (op == 0xb97U || op == 0xb9bU || op == 0x593U || op == 0x59fU) {
            const u32 bank = normalize_bank(insn.Bits(54, 5));
            const u32 offset = insn.Bits(40, 14) * 4U;
            if (bank < Tegra::Engines::Maxwell3D::Regs::MaxConstBuffers) {
                PreprobeNativeCbuf(bank, offset, false);
            }
        }
    }

    snap.cbuf_values = cbuf_values;
    snap.texture_types = texture_types;
    snap.texture_pixel_formats = texture_pixel_formats;
    snap.storage_pointer_types = storage_pointer_types;
    snap.cbuf_replacements = cbuf_replacements;
    return snap;
}

std::shared_ptr<const LoweredNativeCode> GenericEnvironment::CaptureNativeCode(
    u32 source_header_size, bool geometry) {
    using Shader::Ampere::Instruction;
    const u32 native_header_size = source_header_size;
    std::vector<Instruction> native = CaptureNativeInstructions(source_header_size);
    CheckNativeRejectAddress(program_base);

    const u64 program_key = Shader::Ampere::NativeProgramKey(native);
    // EDEN_NX2_SHADER_DUMP=<dir>: keep every captured native program (named by its key) so the
    // lowering or direct compiler can be tested offline against all of a game's shaders.
    if (static const char* const dump_directory = std::getenv("EDEN_NX2_SHADER_DUMP");
        dump_directory != nullptr) {
        const auto folder = std::filesystem::path{dump_directory};
        const auto path = folder / fmt::format("{:016x}-{}-{:x}.bin", program_key,
                                               source_header_size == 0 ? "cs" : "gfx", program_base);
        if (!std::filesystem::exists(path)) {
            std::filesystem::create_directories(folder);
            std::ofstream dump(path, std::ios::binary);
            dump.write(reinterpret_cast<const char*>(native.data()),
                       native.size() * sizeof(Shader::Ampere::Instruction));
        }
    }

    auto result = std::make_shared<LoweredNativeCode>();
    result->native_program_key = program_key;
    static const bool hash_for_indirect_reset = [] {
        const char* value = std::getenv("EDEN_NX2_RESETFIX");
        return !value || std::string_view{value} != "0";
    }();
    if (hash_for_indirect_reset) {
        result->diagnostic_key = result->native_program_key;
    }
    result->source_size = native_header_size + native.size() * sizeof(Shader::Ampere::Instruction);
    std::memcpy(result->header.data(), &sph, sizeof(sph));
    result->native_code = std::move(native);
    result->program_base = program_base;
    result->source_header_size = native_header_size;
    result->geometry = geometry;
    result->native_tensor = native_tensor;
    result->native_compact_tensor = native_compact_tensor;
    result->has_native_context = true;
    result->lazy = std::make_shared<LoweredNativeCode::LazyLoweringState>();
    result->native_hash = ComputeNativeDiscoveryHash(*result);

    if (VideoCommon::Nx2Diag::Active()) {
        VideoCommon::Nx2Diag::Diagnostics::Get().RegisterProgram(
            program_base, result->native_program_key, result->native_hash);
    }

    CheckNativeRejectHashes(program_base, {result->native_program_key, result->native_hash});

    LOG_DEBUG(Shader, "Captured native shader at {:#x}: {} source bytes, {} SM86 instructions",
              program_base, result->source_size, result->native_code.size());
    return result;
}

std::shared_ptr<const LoweredNativeCode> GenericEnvironment::LowerNativeCode(u32 source_header_size,
                                                                            bool geometry) {
    auto captured = CaptureNativeCode(source_header_size, geometry);
    EnsureLoweredNativeCodeImpl(captured, gpu_memory);
    return captured;
}

GraphicsEnvironment::GraphicsEnvironment(Tegra::Engines::Maxwell3D& maxwell3d_,
                                         Tegra::MemoryManager& gpu_memory_,
                                         Maxwell::ShaderType program, GPUVAddr program_base_,
                                         u32 start_address_,
                                         std::shared_ptr<const LoweredNativeCode> lowered_native_)
    : GenericEnvironment{gpu_memory_, program_base_, start_address_}, maxwell3d{&maxwell3d_} {
    gpu_memory->ReadBlock(program_base + start_address, &sph, sizeof(sph));
    initial_offset = sizeof(sph);
    texture_bound = maxwell3d->regs.bindless_texture_const_buffer_slot;
    const auto& regs{maxwell3d->regs};
    const bool native{maxwell3d->GetNativeMacroEngine().native_turing};
    static const bool geometry_enabled = [] {
        const char* value = std::getenv("EDEN_NX2_EXPERIMENTAL_GEOMETRY");
        return value != nullptr && std::string_view{value} == "1";
    }();
    const bool geometry{native && program == Maxwell::ShaderType::Geometry && geometry_enabled};
    // Task and mesh programs: the task in the vertex slot, its mesh program in the tessellation
    // slot (without a task, the mesh program is in the vertex slot).
    const bool mesh_slot{native && regs.IsMeshEnabled() &&
                         (program == Maxwell::ShaderType::VertexB ||
                          program == Maxwell::ShaderType::Tessellation)};
    if (native) {
        if (program != Maxwell::ShaderType::VertexB && program != Maxwell::ShaderType::Pixel &&
            !mesh_slot && !geometry) {
            throw Shader::RuntimeError("Unsupported native graphics shader stage {}", static_cast<u32>(program));
        }
        if (std::getenv("EDEN_NATIVE_DRAW_DIAGNOSTICS")) {
            const u32 native_stage = program == Maxwell::ShaderType::Pixel ? 4 : 0;
            const auto& table = maxwell3d->state.shader_stages[native_stage].const_buffers[1];
            if (table.enabled && table.size && table.size <= Tegra::Engines::Maxwell3D::Regs::MaxConstBufferSize) {
                std::vector<u8> bytes(table.size);
                gpu_memory->ReadBlock(table.address, bytes.data(), bytes.size());
                std::ofstream dump(fmt::format("native-graphics-cbuf-{}-1.bin", static_cast<u32>(program)), std::ios::binary);
                dump.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            }
        }
        InitializeNativeCode(std::move(lowered_native_), 128, geometry);
    }
    gp_passthrough_mask = maxwell3d->regs.post_vtg_shader_attrib_skip_mask;
    switch (program) {
    case Maxwell::ShaderType::VertexA:
        stage = Shader::Stage::VertexA;
        stage_index = 0;
        break;
    case Maxwell::ShaderType::VertexB:
        stage = Shader::Stage::VertexB;
        stage_index = 0;
        break;
    case Maxwell::ShaderType::TessellationInit:
        stage = Shader::Stage::TessellationControl;
        stage_index = 1;
        break;
    case Maxwell::ShaderType::Tessellation:
        stage = Shader::Stage::TessellationEval;
        stage_index = 2;
        break;
    case Maxwell::ShaderType::Geometry:
        stage = Shader::Stage::Geometry;
        stage_index = 3;
        break;
    case Maxwell::ShaderType::Pixel:
        stage = Shader::Stage::Fragment;
        stage_index = 4;
        break;
    default:
        ASSERT_MSG(false, "Invalid program={}", program);
        break;
    }
    if (mesh_slot) {
        const bool has_task{regs.IsShaderConfigEnabled(Maxwell::ShaderType::Tessellation)};
        stage = program == Maxwell::ShaderType::VertexB && has_task ? Shader::Stage::Task
                                                                    : Shader::Stage::Mesh;
        mesh_registers = {regs.reg_array[Maxwell::MeshShaderA], regs.reg_array[Maxwell::MeshShaderB],
                          regs.reg_array[Maxwell::MeshInitShader]};
        if (stage == Shader::Stage::Mesh && regs.IsShaderConfigEnabled(Maxwell::ShaderType::Geometry)) {
            const auto& geometry_pipeline{
                regs.pipelines[static_cast<size_t>(Maxwell::ShaderType::Geometry)]};
            const GPUVAddr address{(static_cast<u64>(geometry_pipeline.reserved[0] & 0xff) << 32) |
                                   geometry_pipeline.reserved[1]};
            gpu_memory->ReadBlock(address, &mesh_primitive_header, sizeof(mesh_primitive_header));
        }
    }
    const u64 local_size{sph.LocalMemorySize()};
    ASSERT(local_size <= (std::numeric_limits<u32>::max)());
    local_memory_size = static_cast<u32>(local_size) + sph.common3.shader_local_memory_crs_size;
    is_proprietary_driver = maxwell3d->GetNativeMacroEngine().native_turing || texture_bound == 2;
    has_hle_engine_state =
        maxwell3d->engine_state == Tegra::Engines::Maxwell3D::EngineHint::OnHLEMacro;
}

u32 GraphicsEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    u32 value{};
    if (snapshot) {
        const auto& cbuf{snapshot->const_buffers[cbuf_index]};
        ASSERT(cbuf.enabled);
        if (cbuf_offset < cbuf.size) {
            std::memcpy(&value, cbuf.bytes.data() + cbuf_offset, sizeof(value));
        }
    } else {
        const auto& cbuf{maxwell3d->state.shader_stages[stage_index].const_buffers[cbuf_index]};
        ASSERT(cbuf.enabled);
        if (cbuf_offset < cbuf.size) {
            value = gpu_memory->Read<u32>(cbuf.address + cbuf_offset);
        }
    }
    cbuf_values.emplace(MakeCbufKey(cbuf_index, cbuf_offset), value);
    return value;
}

std::optional<Shader::ReplaceConstant> GraphicsEnvironment::GetReplaceConstBuffer(u32 bank,
                                                                                  u32 offset) {
    if (!has_hle_engine_state) {
        return std::nullopt;
    }
    const u64 key = (static_cast<u64>(bank) << 32) | static_cast<u64>(offset);
    const auto& replace_table{snapshot ? snapshot->replace_table : maxwell3d->replace_table};
    auto it = replace_table.find(key);
    if (it == replace_table.end()) {
        return std::nullopt;
    }
    const auto converted_value = [](Tegra::Engines::Maxwell3D::HLEReplacementAttributeType name) {
        switch (name) {
        case Tegra::Engines::Maxwell3D::HLEReplacementAttributeType::BaseVertex:
            return Shader::ReplaceConstant::BaseVertex;
        case Tegra::Engines::Maxwell3D::HLEReplacementAttributeType::BaseInstance:
            return Shader::ReplaceConstant::BaseInstance;
        case Tegra::Engines::Maxwell3D::HLEReplacementAttributeType::DrawID:
            return Shader::ReplaceConstant::DrawID;
        default:
            UNREACHABLE();
        }
    }(it->second);
    cbuf_replacements.emplace(key, converted_value);
    return converted_value;
}

Shader::TextureType GraphicsEnvironment::ReadTextureType(u32 handle) {
    const auto entry{ReadTextureDescriptor(handle)};
    const Shader::TextureType result{ConvertTextureType(entry)};
    texture_types.emplace(handle, result);
    return result;
}

Shader::TexturePixelFormat GraphicsEnvironment::ReadTexturePixelFormat(u32 handle) {
    const auto entry{ReadTextureDescriptor(handle)};
    const Shader::TexturePixelFormat result(ConvertTexturePixelFormat(entry));
    texture_pixel_formats.emplace(handle, result);
    return result;
}

bool GraphicsEnvironment::IsTexturePixelFormatInteger(u32 handle) {
    return VideoCore::Surface::IsPixelFormatInteger(
        static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
}

u32 GraphicsEnvironment::ReadViewportTransformState() {
    if (snapshot) {
        viewport_transform_state = snapshot->viewport_scale_offset_enabled;
    } else if (maxwell3d != nullptr) {
        viewport_transform_state = maxwell3d->regs.viewport_scale_offset_enabled;
    }
    return viewport_transform_state;
}

Tegra::Texture::TICEntry GraphicsEnvironment::ReadTextureDescriptor(u32 handle) {
    if (!snapshot) {
        const auto& regs{maxwell3d->regs};
        const bool via_header_index{regs.sampler_binding ==
                                    Maxwell::SamplerBinding::ViaHeaderBinding};
        return ReadTextureInfo(regs.tex_header.Address(), regs.tex_header.limit,
                               via_header_index, handle);
    }
    return ReadSnapshotDescriptor(handle);
}

Tegra::Texture::TICEntry GraphicsEnvironment::ReadSnapshotDescriptor(u32 handle) const {
    const auto pair{Tegra::Texture::TexturePair(handle, snapshot->via_header_index)};
    const u32 index{Tegra::Texture::TextureHeaderIndex(pair.first, snapshot->texture_pool_limit,
                                                       snapshot->native_tiling)};
    ASSERT(index <= snapshot->texture_pool_limit);
    Tegra::Texture::TICEntry entry{};
    if (index <= snapshot->texture_pool_limit) {
        // Only the pool's own range was checked to be contiguous: never read past it.
        snapshot->device_memory->ReadBlockUnsafe(snapshot->texture_pool + index * sizeof(entry),
                                                 &entry, sizeof(entry));
    }
    return entry;
}

// Runs on the GPU thread before the environment goes to a translation worker.
// - Enabled constant buffers are copied whole: translation folds some of their values into the
//   shader (bindless handles, storage pointers, branch tables), and uniform memory is reused once
//   the GPU is past the draw. The copy reads like Read<u32> does, without flushing.
// - The texture pool is located, not copied, and read on the worker. A pool the GPU wrote is not
//   detached (its entries would need a flush, which only the GPU thread may do); an entry the CPU
//   rewrites meanwhile gives the newer descriptor, an accepted risk as pools hold long-lived
//   descriptors.
// - The viewport transform flag and the HLE replacements are copied.
bool GraphicsEnvironment::Detach(Tegra::MaxwellDeviceMemoryManager& device_memory) {
    // A handle indexes at most 2^20 descriptors; a larger limit is not a real pool and would make
    // the checks below walk gigabytes of page table.
    constexpr u32 MaxTexturePoolLimit = (1U << 20) - 1;
    const auto& regs{maxwell3d->regs};
    if (regs.tex_header.limit > MaxTexturePoolLimit) {
        return false;
    }
    const GPUVAddr pool_address{regs.tex_header.Address()};
    const size_t pool_size{(size_t{regs.tex_header.limit} + 1) * sizeof(Tegra::Texture::TICEntry)};
    const std::optional<DAddr> pool_device_address{gpu_memory->GpuToCpuAddress(pool_address)};
    if (!pool_device_address || !gpu_memory->IsContinuousRange(pool_address, pool_size) ||
        gpu_memory->IsMemoryDirty(pool_address, pool_size)) {
        return false;
    }
    EngineSnapshot copy;
    const auto& live_buffers{maxwell3d->state.shader_stages[stage_index].const_buffers};
    for (size_t index = 0; index < live_buffers.size(); ++index) {
        const auto& live{live_buffers[index]};
        auto& copied{copy.const_buffers[index]};
        copied.enabled = live.enabled;
        if (!live.enabled || live.size == 0) {
            continue;
        }
        // The size register can exceed what a constant buffer holds; never copy more than that.
        copied.size = (std::min)(live.size, static_cast<u32>(Maxwell::MaxConstBufferSize));
        copied.bytes.resize(copied.size + sizeof(u32));
        gpu_memory->ReadBlockUnsafe(live.address, copied.bytes.data(), copied.size);
    }
    copy.device_memory = &device_memory;
    copy.texture_pool_gpu = pool_address;
    copy.texture_pool = *pool_device_address;
    copy.texture_pool_limit = regs.tex_header.limit;
    copy.via_header_index = regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding;
    copy.native_tiling = gpu_memory->UsesNativeTiling();
    copy.viewport_scale_offset_enabled = regs.viewport_scale_offset_enabled;
    if (has_hle_engine_state) {
        copy.replace_table = maxwell3d->replace_table;
    }
    snapshot = std::move(copy);
    maxwell3d = nullptr;
    return true;
}

size_t GraphicsEnvironment::SnapshotSizeBytes() const noexcept {
    if (!snapshot) {
        return 0;
    }
    size_t total{};
    for (const auto& cbuf : snapshot->const_buffers) {
        total += cbuf.bytes.size();
    }
    return total;
}

void GraphicsEnvironment::ReleaseSnapshotBuffers() noexcept {
    if (!snapshot) {
        return;
    }
    for (auto& cbuf : snapshot->const_buffers) {
        std::vector<u8>{}.swap(cbuf.bytes);
        cbuf.size = 0;
    }
}

u32 GraphicsEnvironment::TexturePoolEntries() const noexcept {
    return snapshot ? snapshot->texture_pool_limit + 1 : 0;
}

GraphicsEnvironment::TexturePoolCheck GraphicsEnvironment::CheckTextureReads(
    const Tegra::MemoryManager& current) const {
    TexturePoolCheck check;
    if (!snapshot || (texture_types.empty() && texture_pixel_formats.empty())) {
        return check;
    }
    const size_t pool_size{(size_t{snapshot->texture_pool_limit} + 1) *
                           sizeof(Tegra::Texture::TICEntry)};
    const auto device_address{current.GpuToCpuAddress(snapshot->texture_pool_gpu)};
    if (&current != gpu_memory || !device_address || *device_address != snapshot->texture_pool ||
        !current.IsContinuousRange(snapshot->texture_pool_gpu, pool_size)) {
        check.remapped = true;
        return check;
    }
    if (current.IsMemoryDirty(snapshot->texture_pool_gpu, pool_size)) {
        check.dirty = true;
        return check;
    }
    for (const auto& [handle, type] : texture_types) {
        ++check.reads;
        check.changed += ConvertTextureType(ReadSnapshotDescriptor(handle)) != type ? 1 : 0;
    }
    for (const auto& [handle, format] : texture_pixel_formats) {
        ++check.reads;
        check.changed +=
            ConvertTexturePixelFormat(ReadSnapshotDescriptor(handle)) != format ? 1 : 0;
    }
    return check;
}

void GraphicsEnvironment::PreprobeNativeCbuf(u32 cbuf_index, u32 cbuf_offset,
                                             bool probe_pointer_slot) {
    if (cbuf_index >= Maxwell::MaxConstBuffers) {
        return;
    }
    u32 cbuf_size = 0;
    bool can_read = false;
    bool can_read_u64 = false;
    if (snapshot) {
        const auto& cbuf{snapshot->const_buffers[cbuf_index]};
        if (cbuf.enabled) {
            cbuf_size = cbuf.size;
            can_read = cbuf_offset + sizeof(u32) <= cbuf_size;
            can_read_u64 = cbuf_offset + sizeof(u64) <= cbuf_size;
        }
    } else if (maxwell3d != nullptr && gpu_memory != nullptr) {
        const auto& cbuf{maxwell3d->state.shader_stages[stage_index].const_buffers[cbuf_index]};
        if (cbuf.enabled) {
            cbuf_size = cbuf.size;
            can_read = cbuf_offset + sizeof(u32) <= cbuf_size &&
                       gpu_memory->IsFullyMappedRange(cbuf.address + cbuf_offset, sizeof(u32));
            can_read_u64 = cbuf_offset + sizeof(u64) <= cbuf_size &&
                           gpu_memory->IsFullyMappedRange(cbuf.address + cbuf_offset, sizeof(u64));
        }
    }
    if (!can_read) {
        return;
    }
    static_cast<void>(ReadCbufValue(cbuf_index, cbuf_offset));
    if (has_hle_engine_state) {
        static_cast<void>(GetReplaceConstBuffer(cbuf_index, cbuf_offset));
    }
    if (probe_pointer_slot && can_read_u64) {
        static_cast<void>(IsCbufStorageBufferPointer(cbuf_index, cbuf_offset));
    }
}

void GraphicsEnvironment::PreprobeNativeTexture(u32 cbuf_index, u32 cbuf_offset) {
    if (snapshot) {
        const auto it = cbuf_values.find(MakeCbufKey(cbuf_index, cbuf_offset));
        if (it == cbuf_values.end()) {
            return;
        }
        const u32 handle = it->second;
        const auto pair{Tegra::Texture::TexturePair(handle, snapshot->via_header_index)};
        const u32 index{Tegra::Texture::TextureHeaderIndex(
            pair.first, snapshot->texture_pool_limit, snapshot->native_tiling)};
        if (index > snapshot->texture_pool_limit) {
            return;
        }
        const Tegra::Texture::TICEntry entry = ReadSnapshotDescriptor(handle);
        if (static_cast<u32>(entry.texture_type.Value()) >
            static_cast<u32>(Tegra::Texture::TextureType::TextureCubeArray)) {
            return;
        }
        static_cast<void>(ReadTextureType(handle));
        static_cast<void>(ReadTexturePixelFormat(handle));
    } else if (maxwell3d != nullptr) {
        const auto& regs{maxwell3d->regs};
        const bool via_header_index{regs.sampler_binding ==
                                    Maxwell::SamplerBinding::ViaHeaderBinding};
        TryPreprobeTextureFromPool(regs.tex_header.Address(), regs.tex_header.limit,
                                   via_header_index, cbuf_index, cbuf_offset);
    }
}

ComputeEnvironment::ComputeEnvironment(Tegra::Engines::KeplerCompute& kepler_compute_,
                                       Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                       u32 start_address_,
                                       std::shared_ptr<const LoweredNativeCode> lowered_native_)
    : GenericEnvironment{gpu_memory_, program_base_, start_address_}, kepler_compute{
                                                                          &kepler_compute_} {
    const auto& qmd{kepler_compute->launch_description};
    stage = Shader::Stage::Compute;
    local_memory_size = qmd.local_pos_alloc + qmd.local_crs_alloc;
    texture_bound = kepler_compute->regs.tex_cb_index;
    is_proprietary_driver = texture_bound == 2;
    shared_memory_size = qmd.shared_alloc;
    workgroup_size = {qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z};
    if (kepler_compute->IsNativeMode()) {
        initial_offset = sizeof(sph);
        is_proprietary_driver = true;
        native_tensor = kepler_compute->EmulatesNativeTensorPrograms();
        native_compact_tensor = kepler_compute->UsesCompactTensorPrograms();
        InitializeNativeCode(std::move(lowered_native_), 0);
        start_address = sizeof(sph);
    }
}

u32 ComputeEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    const auto& qmd{kepler_compute->launch_description};
    ASSERT(((qmd.const_buffer_enable_mask.Value() >> cbuf_index) & 1) != 0);
    const auto& cbuf{qmd.const_buffer_config[cbuf_index]};
    u32 value{};
    if (cbuf_offset < cbuf.size) {
        value = gpu_memory->Read<u32>(cbuf.Address() + cbuf_offset);
    }
    cbuf_values.emplace(MakeCbufKey(cbuf_index, cbuf_offset), value);
    return value;
}

Shader::TextureType ComputeEnvironment::ReadTextureType(u32 handle) {
    const auto& regs{kepler_compute->regs};
    const auto& qmd{kepler_compute->launch_description};
    auto entry = ReadTextureInfo(regs.tic.Address(), regs.tic.limit, qmd.linked_tsc != 0, handle);
    const Shader::TextureType result{ConvertTextureType(entry)};
    texture_types.emplace(handle, result);
    return result;
}

Shader::TexturePixelFormat ComputeEnvironment::ReadTexturePixelFormat(u32 handle) {
    const auto& regs{kepler_compute->regs};
    const auto& qmd{kepler_compute->launch_description};
    auto entry = ReadTextureInfo(regs.tic.Address(), regs.tic.limit, qmd.linked_tsc != 0, handle);
    const Shader::TexturePixelFormat result(ConvertTexturePixelFormat(entry));
    texture_pixel_formats.emplace(handle, result);
    return result;
}

bool ComputeEnvironment::IsTexturePixelFormatInteger(u32 handle) {
    return VideoCore::Surface::IsPixelFormatInteger(
        static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
}

u32 ComputeEnvironment::ReadViewportTransformState() {
    return viewport_transform_state;
}

void ComputeEnvironment::PreprobeNativeCbuf(u32 cbuf_index, u32 cbuf_offset,
                                            bool probe_pointer_slot) {
    if (kepler_compute == nullptr || gpu_memory == nullptr) {
        return;
    }
    const auto& qmd{kepler_compute->launch_description};
    if (cbuf_index >= qmd.const_buffer_config.size() ||
        ((qmd.const_buffer_enable_mask.Value() >> cbuf_index) & 1U) == 0U) {
        return;
    }
    const auto& cbuf{qmd.const_buffer_config[cbuf_index]};
    if (cbuf_offset + sizeof(u32) > cbuf.size ||
        !gpu_memory->IsFullyMappedRange(cbuf.Address() + cbuf_offset, sizeof(u32))) {
        return;
    }
    static_cast<void>(ReadCbufValue(cbuf_index, cbuf_offset));
    if (probe_pointer_slot && cbuf_offset + sizeof(u64) <= cbuf.size &&
        gpu_memory->IsFullyMappedRange(cbuf.Address() + cbuf_offset, sizeof(u64))) {
        static_cast<void>(IsCbufStorageBufferPointer(cbuf_index, cbuf_offset));
    }
}

void ComputeEnvironment::PreprobeNativeTexture(u32 cbuf_index, u32 cbuf_offset) {
    if (kepler_compute == nullptr) {
        return;
    }
    const auto& regs{kepler_compute->regs};
    const auto& qmd{kepler_compute->launch_description};
    TryPreprobeTextureFromPool(regs.tic.Address(), regs.tic.limit, qmd.linked_tsc != 0U,
                               cbuf_index, cbuf_offset);
}

void FileEnvironment::Deserialize(std::ifstream& file, bool with_mesh_state) {
    lowered_native.reset();
    native_has_hle_engine_state = false;
    u64 code_size{};
    u64 num_texture_types{};
    u64 num_texture_pixel_formats{};
    u64 num_cbuf_values{};
    u64 num_storage_pointer_types{};
    u64 num_cbuf_replacement_values{};
    file.read(reinterpret_cast<char*>(&code_size), sizeof(code_size))
        .read(reinterpret_cast<char*>(&num_texture_types), sizeof(num_texture_types))
        .read(reinterpret_cast<char*>(&num_texture_pixel_formats),
              sizeof(num_texture_pixel_formats))
        .read(reinterpret_cast<char*>(&num_cbuf_values), sizeof(num_cbuf_values))
        .read(reinterpret_cast<char*>(&num_storage_pointer_types),
              sizeof(num_storage_pointer_types))
        .read(reinterpret_cast<char*>(&num_cbuf_replacement_values),
              sizeof(num_cbuf_replacement_values))
        .read(reinterpret_cast<char*>(&local_memory_size), sizeof(local_memory_size))
        .read(reinterpret_cast<char*>(&texture_bound), sizeof(texture_bound))
        .read(reinterpret_cast<char*>(&start_address), sizeof(start_address))
        .read(reinterpret_cast<char*>(&read_lowest), sizeof(read_lowest))
        .read(reinterpret_cast<char*>(&read_highest), sizeof(read_highest))
        .read(reinterpret_cast<char*>(&viewport_transform_state), sizeof(viewport_transform_state))
        .read(reinterpret_cast<char*>(&stage), sizeof(stage));
    code.resize(Common::DivCeil(code_size, sizeof(u64)));
    file.read(reinterpret_cast<char*>(code.data()), code_size);
    for (size_t i = 0; i < num_texture_types; ++i) {
        u32 key;
        Shader::TextureType type;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&type), sizeof(type));
        texture_types.emplace(key, type);
    }
    for (size_t i = 0; i < num_texture_pixel_formats; ++i) {
        u32 key;
        Shader::TexturePixelFormat format;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&format), sizeof(format));
        texture_pixel_formats.emplace(key, format);
    }
    for (size_t i = 0; i < num_cbuf_values; ++i) {
        u64 key;
        u32 value;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&value), sizeof(value));
        cbuf_values.emplace(key, value);
    }
    for (size_t i = 0; i < num_storage_pointer_types; ++i) {
        u64 key;
        u8 value;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&value), sizeof(value));
        storage_pointer_types.emplace(key, value);
    }
    for (size_t i = 0; i < num_cbuf_replacement_values; ++i) {
        u64 key;
        Shader::ReplaceConstant value;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&value), sizeof(value));
        cbuf_replacements.emplace(key, value);
    }
    if (stage == Shader::Stage::Compute) {
        file.read(reinterpret_cast<char*>(&workgroup_size), sizeof(workgroup_size))
            .read(reinterpret_cast<char*>(&shared_memory_size), sizeof(shared_memory_size));
        initial_offset = Shader::Ampere::HasNativeAddressMap(code) ? sizeof(sph) : 0;
    } else {
        file.read(reinterpret_cast<char*>(&sph), sizeof(sph));
        initial_offset = sizeof(sph);
        if (stage == Shader::Stage::Geometry) {
            file.read(reinterpret_cast<char*>(&gp_passthrough_mask), sizeof(gp_passthrough_mask));
        }
        if (with_mesh_state && (stage == Shader::Stage::Task || stage == Shader::Stage::Mesh)) {
            file.read(reinterpret_cast<char*>(&mesh_registers), sizeof(mesh_registers))
                .read(reinterpret_cast<char*>(&mesh_primitive_header),
                      sizeof(mesh_primitive_header));
        }
    }
    auto unpacked = std::make_shared<LoweredNativeCode>();
    bool has_hle_flag{};
    if (TryUnpackNativeSm86Blob(code, *unpacked, has_hle_flag)) {
        lowered_native = std::move(unpacked);
        native_has_hle_engine_state = has_hle_flag;
        code.clear();
        read_lowest = 0;
        read_highest = 0;
        initial_offset = sizeof(sph);
        is_proprietary_driver = true;
        return;
    }
    // Native programs fold bank 1 reads as they did when they were built.
    is_proprietary_driver = texture_bound == 2 || Shader::Ampere::HasNativeAddressMap(code);
}

bool FileEnvironment::HasNativeInstructions() const noexcept {
    return lowered_native && !lowered_native->native_code.empty();
}

void FileEnvironment::EnsureLoweredNative() {
    if (!code.empty() || !lowered_native) {
        return;
    }
    EnsureLoweredNativeCodeImpl(lowered_native, nullptr);
    const auto words = lowered_native->LoweredWords();
    if (words.empty()) {
        throw Shader::RuntimeError("Lowered cached native shader is empty");
    }
    code.assign(words.begin(), words.end());
    read_lowest = 0;
    read_highest = static_cast<u32>((code.size() - 1) * sizeof(u64));
    if (const u32 tb = lowered_native->EffectiveTextureBound(); tb != ~0U) {
        texture_bound = tb;
    }
}

NativeShaderSnapshot FileEnvironment::BuildNativeSnapshot() const {
    NativeShaderSnapshot snap;
    if (stage == Shader::Stage::Compute && lowered_native) {
        snap.header = lowered_native->header;
    } else {
        std::memcpy(snap.header.data(), &sph, sizeof(sph));
    }
    if (lowered_native) {
        snap.native_code = lowered_native->native_code;
        snap.program_key = lowered_native->native_program_key;
        snap.native_tensor = lowered_native->native_tensor;
        snap.native_compact_tensor = lowered_native->native_compact_tensor;
    }
    snap.stage = stage;
    snap.local_memory_size = local_memory_size;
    snap.shared_memory_size = shared_memory_size;
    snap.workgroup_size = workgroup_size;
    snap.texture_bound = TextureBoundBuffer();
    snap.viewport_transform_state = viewport_transform_state;
    snap.has_hle_engine_state = HasHLEMacroState();
    snap.mesh_registers = mesh_registers;
    snap.mesh_primitive_header = mesh_primitive_header;
    snap.cbuf_values = cbuf_values;
    snap.texture_types = texture_types;
    snap.texture_pixel_formats = texture_pixel_formats;
    snap.storage_pointer_types = storage_pointer_types;
    snap.cbuf_replacements = cbuf_replacements;
    return snap;
}

void FileEnvironment::Dump(u64 pipeline_hash, u64 shader_hash) {
    if (code.empty()) {
        if (!lowered_native || !lowered_native->HasLoweredCode()) {
            return;
        }
        const auto words = lowered_native->LoweredWords();
        DumpImpl(pipeline_hash, shader_hash, words, read_highest, read_lowest, initial_offset,
                 stage);
        return;
    }
    DumpImpl(pipeline_hash, shader_hash, code, read_highest, read_lowest, initial_offset, stage);
}

u32 FileEnvironment::TranslateProgramAddress(u32 address) const {
    if (!code.empty()) {
        if (const auto translated = Shader::Ampere::NativeProgramAddress(code, address)) {
            return *translated;
        }
        if (Shader::Ampere::HasNativeAddressMap(code)) {
            throw Shader::RuntimeError("Invalid cached native indirect branch address {:#x}",
                                       address);
        }
        return address;
    }
    if (lowered_native) {
        EnsureLoweredNativeCodeImpl(lowered_native, nullptr);
        if (const auto translated =
                Shader::Ampere::NativeProgramAddress(lowered_native->LoweredWords(), address)) {
            return *translated;
        }
        throw Shader::RuntimeError("Invalid cached native indirect branch address {:#x}", address);
    }
    return address;
}

bool FileEnvironment::IsNativeProgram() const {
    return HasNativeInstructions() || Shader::Ampere::HasNativeAddressMap(code);
}

std::vector<std::pair<u32, u32>> FileEnvironment::NullStoragePointerSlots() const {
    return VideoCommon::NullStoragePointerSlots(cbuf_values, storage_pointer_types);
}

std::vector<u64> FileEnvironment::MappedStoragePointerKeys() const {
    return VideoCommon::MappedStoragePointerKeys(storage_pointer_types);
}

/// Recorded classification of the constant buffer word at `index`/`offset` as a storage buffer
/// pointer. Without a record, the guess when one is set, else throws.
bool FileEnvironment::IsCbufStorageBufferPointer(u32 index, u32 offset) {
    const auto it = storage_pointer_types.find(MakeCbufKey(index, offset));
    if (it == storage_pointer_types.end()) {
        if (guess) {
            return index == 0 ? guess->storage_pointer_bank0 : guess->storage_pointer_other;
        }
        throw Shader::LogicError("Uncached storage pointer classification");
    }
    return it->second != 0;
}

/// Same as IsCbufStorageBufferPointer for a pointer read through an indirect table.
bool FileEnvironment::IsCbufIndirectStorageBufferPointer(u32 index, u32 offset, u32 indirect) {
    const auto it = storage_pointer_types.find(MakeIndirectCbufKey(index, offset, indirect));
    if (it == storage_pointer_types.end()) {
        if (guess) {
            return index == 0 ? guess->storage_pointer_bank0 : guess->storage_pointer_other;
        }
        throw Shader::LogicError("Uncached storage pointer table classification");
    }
    return it->second != 0;
}

u64 FileEnvironment::ReadInstruction(u32 address) {
    if (code.empty() && lowered_native) {
        EnsureLoweredNative();
    }
    if (address < read_lowest || address > read_highest) {
        throw Shader::LogicError("Out of bounds address {}", address);
    }
    return code[(address - read_lowest) / sizeof(u64)];
}

/// Recorded constant buffer value at `cbuf_index`/`cbuf_offset`. Without a record, the guess when
/// one is set, else throws.
u32 FileEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    const auto it{cbuf_values.find(MakeCbufKey(cbuf_index, cbuf_offset))};
    if (it == cbuf_values.end()) {
        if (guess) {
            return cbuf_index == 0 ? guess->cbuf_bank0 : guess->cbuf_other;
        }
        throw Shader::LogicError("Uncached read texture type");
    }
    return it->second;
}

/// Recorded texture type of `handle`. Without a record, the guess when one is set, else throws.
Shader::TextureType FileEnvironment::ReadTextureType(u32 handle) {
    const auto it{texture_types.find(handle)};
    if (it == texture_types.end()) {
        if (guess) {
            return guess->texture_type;
        }
        throw Shader::LogicError("Uncached read texture type");
    }
    return it->second;
}

/// Recorded texture pixel format of `handle`. Without a record, the guess when one is set, else
/// throws.
Shader::TexturePixelFormat FileEnvironment::ReadTexturePixelFormat(u32 handle) {
    const auto it{texture_pixel_formats.find(handle)};
    if (it == texture_pixel_formats.end()) {
        if (guess) {
            return guess->pixel_format;
        }
        throw Shader::LogicError("Uncached read texture pixel format");
    }
    return it->second;
}

bool FileEnvironment::IsTexturePixelFormatInteger(u32 handle) {
    return VideoCore::Surface::IsPixelFormatInteger(
        static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
}

u32 FileEnvironment::ReadViewportTransformState() {
    return viewport_transform_state;
}

u32 FileEnvironment::LocalMemorySize() const {
    return local_memory_size;
}

u32 FileEnvironment::SharedMemorySize() const {
    return shared_memory_size;
}

u32 FileEnvironment::TextureBoundBuffer() const {
    if (code.empty() && lowered_native) {
        if (const u32 tb = lowered_native->EffectiveTextureBound(); tb != ~0U) {
            return tb;
        }
    }
    return texture_bound;
}

std::array<u32, 3> FileEnvironment::WorkgroupSize() const {
    return workgroup_size;
}

std::optional<Shader::ReplaceConstant> FileEnvironment::GetReplaceConstBuffer(u32 bank,
                                                                              u32 offset) {
    const u64 key = (static_cast<u64>(bank) << 32) | static_cast<u64>(offset);
    auto it = cbuf_replacements.find(key);
    if (it == cbuf_replacements.end()) {
        return std::nullopt;
    }
    return it->second;
}

void SerializePipeline(std::span<const char> key, std::span<const GenericEnvironment* const> envs,
                       const std::filesystem::path& filename, u32 cache_version,
                       bool with_mesh_state) try {
    std::ofstream file(filename, std::ios::binary | std::ios::ate | std::ios::app);
    file.exceptions(std::ifstream::failbit);
    if (!file.is_open()) {
        LOG_ERROR(Common_Filesystem, "Failed to open pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
        return;
    }
    if (file.tellp() == 0) {
        // Write header
        file.write(MAGIC_NUMBER.data(), MAGIC_NUMBER.size())
            .write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version));
    }
    if (!std::ranges::all_of(envs, &GenericEnvironment::CanBeSerialized)) {
        return;
    }
    const u32 num_envs{static_cast<u32>(envs.size())};
    file.write(reinterpret_cast<const char*>(&num_envs), sizeof(num_envs));
    for (const GenericEnvironment* const env : envs) {
        env->Serialize(file, with_mesh_state);
    }
    file.write(key.data(), key.size_bytes());

} catch (const std::ios_base::failure& e) {
    LOG_ERROR(Common_Filesystem, "{}", e.what());
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
}

void LoadPipelines(
    std::stop_token stop_loading, const std::filesystem::path& filename, u32 expected_cache_version,
    Common::UniqueFunction<void, std::ifstream&, FileEnvironment> load_compute,
    Common::UniqueFunction<void, std::ifstream&, std::vector<FileEnvironment>> load_graphics,
    bool with_mesh_state, u32 min_compatible_version, CacheUpgrade* upgrade) try {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return;
    }
    file.exceptions(std::ifstream::failbit);
    const auto end{file.tellg()};
    file.seekg(0, std::ios::beg);

    std::array<char, 8> magic_number;
    u32 cache_version;
    file.read(magic_number.data(), magic_number.size())
        .read(reinterpret_cast<char*>(&cache_version), sizeof(cache_version));
    const bool upgrading = upgrade != nullptr && magic_number == MAGIC_NUMBER &&
                           cache_version >= min_compatible_version &&
                           cache_version < expected_cache_version;
    if (upgrading) {
        upgrade->from_version = cache_version;
    }
    if (!upgrading && (magic_number != MAGIC_NUMBER || cache_version != expected_cache_version)) {
        file.close();
        if (cache_version != expected_cache_version && magic_number == MAGIC_NUMBER) {
            std::filesystem::path backup = filename;
            backup += fmt::format(".v{}", cache_version);
            std::error_code ec;
            std::filesystem::rename(filename, backup, ec);
            if (!ec) {
                LOG_INFO(Common_Filesystem, "Renamed old pipeline cache (v{}) to {}",
                         cache_version, Common::FS::PathToUTF8String(backup));
                return;
            }
        }
        if (Common::FS::RemoveFile(filename)) {
            if (magic_number != MAGIC_NUMBER) {
                LOG_ERROR(Common_Filesystem, "Invalid pipeline cache file");
            }
            if (cache_version != expected_cache_version) {
                LOG_INFO(Common_Filesystem, "Deleting old pipeline cache");
            }
        } else {
            LOG_ERROR(Common_Filesystem,
                      "Invalid pipeline cache file and failed to delete it in \"{}\"",
                      Common::FS::PathToUTF8String(filename));
        }
        return;
    }
    while (file.tellg() != end) {
        if (stop_loading.stop_requested()) {
            return;
        }
        const auto record_begin{file.tellg()};
        u32 num_envs{};
        file.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
        std::vector<FileEnvironment> envs(num_envs);
        for (FileEnvironment& env : envs) {
            env.Deserialize(file, with_mesh_state);
        }
        const bool is_compute{envs.front().ShaderStage() == Shader::Stage::Compute};
        if (is_compute) {
            load_compute(file, std::move(envs.front()));
        } else {
            load_graphics(file, std::move(envs));
        }
        if (upgrading) {
            const auto record_end{file.tellg()};
            std::string& record =
                upgrade->records.emplace_back(static_cast<size_t>(record_end - record_begin), '\0');
            file.seekg(record_begin);
            file.read(record.data(), static_cast<std::streamsize>(record.size()));
            upgrade->is_compute.push_back(is_compute);
        }
    }

} catch (const std::ios_base::failure& e) {
    LOG_ERROR(Common_Filesystem, "{}", e.what());
    if (upgrade != nullptr && upgrade->from_version != 0) {
        // The records read so far are written again under the current version.
        return;
    }
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
}

void RewritePipelines(const std::filesystem::path& filename, u32 cache_version, u32 old_version,
                      std::span<const std::string* const> records) try {
    auto temporary{filename};
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.exceptions(std::ofstream::failbit);
        file.write(MAGIC_NUMBER.data(), MAGIC_NUMBER.size())
            .write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version));
        for (const std::string* const record : records) {
            file.write(record->data(), static_cast<std::streamsize>(record->size()));
        }
    }
    auto backup{filename};
    backup += fmt::format(".v{}.bak", old_version);
    std::filesystem::rename(filename, backup);
    std::filesystem::rename(temporary, filename);
    std::error_code error;
    std::filesystem::remove(backup, error);
} catch (const std::exception& e) {
    LOG_ERROR(Common_Filesystem, "Failed to write pipeline cache file {} again: {}",
              Common::FS::PathToUTF8String(filename), e.what());
    // Keep the old file when the new one couldn't take its place.
    auto backup{filename};
    backup += fmt::format(".v{}.bak", old_version);
    std::error_code error;
    if (!std::filesystem::exists(filename, error) && std::filesystem::exists(backup, error)) {
        std::filesystem::rename(backup, filename, error);
    }
}

} // namespace VideoCommon
